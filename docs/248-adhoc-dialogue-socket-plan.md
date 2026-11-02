# Ad-hoc speech over the text-to-dialogue socket

Plan for issue #248. Evidence and measurements live in #247; this is the
implementation shape.

## Why bother

Creature World drives the ad-hoc path dozens of times a day, and it is the one
path with no performed emotion. `eleven_v4_turbo` fixes that, and the
measurements say it is not a latency trade:

| path | model | time to first audio |
|---|---|---|
| text-to-dialogue socket | `eleven_v4_turbo` | **140 ms**, per turn |
| TTS socket (today) | `eleven_flash_v2_5` | **193 ms** median |

Tags are genuinely performed: `[sighs]` produces a 1.3 s sigh, confirmed by
Scribe returning it as an `audio_event` rather than a spoken word. Flash
pronounces the same tag as "Sighs."

## The protocol

Reverse-engineered live; it is not documented for this use.

```
wss://api.elevenlabs.io/v1/text-to-dialogue/stream-input
    ?model_id=eleven_v4_turbo&output_format=pcm_48000&sync_alignment=true

{"voices": ["<voice_id>", ...]}                     first message, bare strings
{"inputs": [{"text": "...", "voice_id": "..."}]}    one or more turns
{"flush": true}                                     releases the pending turn
```

Replies carry `audio` (base64), `alignment`, `normalized_alignment` and
`is_final_audio_for_turn`.

Four things that are easy to get wrong:

- **`voices` must be first, as bare strings.** Objects are rejected with
  `voices.0: Input should be a valid string`; omitting it gives `missing_voices`.
- **`sync_alignment=true` is mandatory for us.** Without it only `audio` comes
  back and lip sync is impossible.
- **`{"flush": true}` is not optional.** A turn's `is_final_audio_for_turn` is
  withheld until the next message arrives, so without a flush exactly
  `inputs - 1` turns ever complete. A single turn with no flush yields no audio
  at all.
- **Key spellings differ from the TTS socket**: `normalized_alignment` /
  `is_final_audio_for_turn` here, `normalizedAlignment` / `isFinal` there.

## The design tension, and the resolution

The dialogue socket looks like a batch API: hand it every turn and the model
sees the whole exchange, which is where the dialog path's inter-turn prosody
comes from. Ad-hoc cannot work that way. `addTurn()` is called as the LLM
produces each sentence, and the playback thread starts turn 1 while turn 2 does
not yet exist. That property is the whole reason ad-hoc feels responsive, and
#248 must not trade it away.

Incremental feeding does work on one connection, but the deferred boundary
means a turn does not finalize until the following turn arrives — turn 1's
`turn_end` landed at 6.41 s when turn 2 was sent at 6.00 s.

**Flush after every turn** gives both properties at once. Measured across three
turns on one connection:

```
t= 0.00  turn 1 + flush -> audio at 0.14s, turn_end 0.73s (3.60s audio)
t= 5.01  turn 2 + flush -> audio at 5.15s, turn_end 5.74s (4.08s audio)
t=10.01  turn 3 + flush -> audio at 10.15s, turn_end 10.62s (3.20s audio)
```

140 ms to first audio on every turn, each turn finalized in well under a
second, the connection alive throughout so later turns keep earlier context,
and a boundary per turn so audio stays separable per creature. Per-creature
channel isolation is non-negotiable for the 17-channel show, and this preserves
it.

## Work, in order

### 1. Generalize `StreamingTTSClient`

It already owns debugged TLS and WebSocket framing over OpenSSL, and carries
the `estimateAudioSeconds` fix from #189 and the startup-frame work from #190.
A sibling class would fork all of that. Add a dialogue-socket mode:

- a second connect path and query string
- `voices` registration, `inputs` and `flush` message builders
- an alignment parser that accepts both key spellings
- a receive loop that returns at `is_final_audio_for_turn` instead of on close

### 2. Hold the connection on the session

`StreamingAdHocSession::start()` already resolves every participant, which is
exactly the moment `voices` has to be registered. One connection per session,
owned by the session, closed in `finish()`.

### 3. Survive the 20 s idle timeout

The socket closes with `input_timeout_exceeded` after 20 s of silence. An
LLM-driven exchange pauses longer than that routinely, so the client reconnects
transparently. A reconnect loses the shared context — a quality regression, not
an error — so it gets a span attribute and we watch how often it happens in
production before deciding whether to keep a keepalive instead.

### 4. One setting, server-side, defaulting to today's behaviour

`adhoc_voice_model` is the model every creature speaks ad-hoc with, and its
value also picks the socket: a tag-capable v4 model routes to text-to-dialogue,
anything else to text-to-speech. One setting rather than a flag plus a model
because the two cannot disagree — the dialogue socket refuses the fast models
with 403 and the text-to-speech socket refuses v3/v4 with 400, so the path
follows the model and a single value can never contradict itself.

It defaults to `eleven_flash_v2_5`, which is what most creatures already used,
so nothing changes until asked.

This replaced the old per-creature `voice.model_id`, which the server no longer
reads at all (issue #250). That block was never strictly validated — readers
pull fields out of the raw JSON in a try/catch and only `voice_id` is required
anywhere — so existing controller JSONs keep working untouched and can be
cleaned up whenever convenient.

Three consequences:

- **Cobalt gains working ad-hoc speech.** It was configured for
  `eleven_multilingual_v2`, which no ad-hoc socket serves, so every ad-hoc turn
  for it failed. That model is now refused at startup with an explanation
  instead of failing silently per turn.
- Caroll and Dev Board 2 come off the deprecated `eleven_turbo_v2_5` without
  anyone editing a controller.
- The job-based ad-hoc path (`handleAdHocSpeechJob` →
  `StreamingSpeechGenerationManager`) cannot use the v4 families on its
  text-to-speech socket, so it declines early and lets its existing REST
  fallback render the job. That fallback does serve v4 — verified against the
  live API — so a v4 setting costs that path its streaming, not its output.

### 5. Leave prosody chaining alone for now

`CreatureContinuity::lastRequestId` chains prosody per voice across turns. A
persistent connection may make it redundant, but that is a separate experiment
from getting the path working.

## Testing

- Unit: message builders, both alignment key spellings, boundary detection,
  `inputs - 1` regression guard (a turn with no flush must not be treated as
  complete).
- The existing ad-hoc suite must pass unchanged with the switch off.
- On real birds: a multi-creature exchange with tags, checking that each
  creature's audio lands on its own channel and that the mouth rests through
  performed tags rather than mouthing them.

## WebSocket client: IXWebSocket, not uWebSockets

The first instinct was to stop hand-rolling and use uWebSockets, now that it is
the transport. That turned out not to be available: `ClientApp.h` in v20.79.0 is
an unimplemented stub — `connect()` has an empty body, the constructor's one
real line is commented out, the callbacks carry no payload so they could not
deliver a message, it has no include guard, and nothing in uWS includes it. The
author's own note inside it reads "needs a new http context with minimal
client". uWebSockets is server-only.

TLS was never the obstacle; the client simply does not exist. `uSockets` does
offer `us_socket_context_connect(int ssl, ...)`, but that is transport only —
the HTTP upgrade and all the framing would still be ours to write, which is the
thing we were trying to stop doing.

So the dialogue client uses **IXWebSocket v11.4.6**, the same client
creature-controller already uses for its own server connection. Unlike the
controller's plain `ws://` LAN link this one needs TLS, so our FetchContent sets
`USE_TLS` and `USE_OPEN_SSL` against the OpenSSL already resolved for
audio-cache hashing.

`DialogueStreamClient` is therefore a separate class from `StreamingTTSClient`
rather than a mode of it: they speak different endpoints over different
transports, and the text-to-speech client's hand-rolled OpenSSL framing carries
production fixes (#189, #190) not worth disturbing. What the two genuinely
share — alignment parsing — is shared, as `wireKeys()` and
`appendAlignmentChunk()` in `StreamingTTSClient.h`. No WebSocket framing is
duplicated anywhere.

Migrating the text-to-speech path onto IXWebSocket too would finish the job of
removing hand-rolled framing from this repo. It is deliberately not part of this
change: that path drives the live show, and it should follow once the dialogue
client has proven itself on real birds.
