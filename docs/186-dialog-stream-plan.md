# Issue #186 — Streaming multi-character ad-hoc dialog session

**Status:** implemented in 3.46.0.
**Issue:** [#186](https://github.com/opsnlops/creature-server/issues/186)

## Problem

Creature World is growing a flock: one `creature-agent` per parrot, coordinated
by the world into *scenes*. A scene is composed turn by turn in text at mind
speed and then performed. Today a multi-voice scene is rendered through
`POST /api/v1/animation/dialog` (`persistence: "adhoc"`, `autoplay: true`) —
one ElevenLabs Text-to-Dialogue render of the whole scene, ~5–10 s after
composition. A single character already gets ~2 s to first words through
`ad-hoc-stream`; April wants the same feel for two birds talking.

Wanted: a streaming session with several creatures, the way `ad-hoc-stream`
streams one. Each turn is one creature's sentence, synthesized in that
creature's voice, lip-synced on that creature's channels, played in arrival
order while the others listen — and, because the world isn't stage-aware, the
server insists on a stage so the birds look at each other.

Trade-off, by design: streamed turns are single-voice renders that do not
react to each other in tone the way Text-to-Dialogue does. The complete-scene
`/dialog` render stays the choice for pre-composed scenes; this is for when
latency matters more than joint conditioning. The world chooses per scene.

## Design

### One session type, N participants

`StreamingAdHocSession` ([StreamingAdHocSession.h](../src/server/voice/StreamingAdHocSession.h))
was generalized from one creature to a cast. A `StreamingSessionConfig`
carries `creatureIds`, an optional `stageId`, `resumePlaylist`, and a `dialog`
flag. `/ad-hoc-stream` builds a one-creature config with `dialog=false` and
behaves exactly as before; `/dialog-stream` sets `dialog=true` and a stage.

`start()` validates every participant the way it validated the one creature
(voice config, streaming-capable model, speech loop), and additionally:

- refuses two participants on one `audio_channel` (they would talk over each
  other in the 17-channel WAV);
- requires every speech loop to run at one `ms/frame` (a turn is one animation,
  and every track of an animation runs at one rate);
- resolves the universe with `resolveCommonUniverse` — every participant must
  be registered on the same one;
- resolves each participant's idle loop (#119) when there is more than one
  participant, so listeners have something to do;
- loads the stage and requires every participant to be placed on it. A
  dialog without a stage, or with an unplaced participant, fails `/start`.

### A turn is one N-track animation

Each turn renders one `Animation` with a track per participant, all
`ceil(audio_ms / ms_per_frame)` frames long:

- **Speaker:** `buildSpeechTrack` in simple mode — speech loop cycled from
  where this creature's loop last stopped, mouth driven by the ElevenLabs
  character alignment.
- **Listeners:** `buildSpeechTrack` in `dialogIdleMode` with no mouth bytes —
  the idle loop cycled from where it last stopped, beak pinned shut; or frozen
  on the speech loop's first frame if the creature has no usable idle loop.

The audio is the speaker's PCM in the speaker's lane of a 17-channel WAV, as
today. Dispatch is unchanged: `SessionManager::interrupt` for turn 1, then
`queueAnimation` chained on the session id, with the direct-schedule fallback
when the chain went idle (#100). A multi-track animation already plays as one
session on one universe — that is how `/dialog` autoplay works.

### Continuity across turns

Each turn resolves a `TurnContinuity` snapshot for the next one (the same
promise/future chain that used to carry just the body offset and the request
id). Per creature: body-loop phase, idle-loop phase, the last body frame
emitted, the creature's own last ElevenLabs request id (prosody chaining is
per voice, not "whoever spoke last"), and its `GazeContinuity`. A failed turn
forwards the snapshot it received, so the chain never stalls.

Two additive extensions to the shared builders make the joins seamless — both
off by default, existing output pinned byte-for-byte by tests:

- `SpeechTrackOptions::entryFadeFrom` ([SpeechTrackBuilder.h](../src/server/voice/SpeechTrackBuilder.h)):
  when a creature changes role (speaking ↔ listening) it is on a different
  loop, so its first frames blend from the previous turn's last body frame.
  `SpeechTrackResult` now also reports `idleEndOffset` and `lastBodyFrame`.
- `GazeContinuity` ([GazeTrack.h](../src/server/voice/GazeTrack.h)):
  `buildGazeTrack` plans a whole scene — it opens on the audience pose, draws
  the favoured cock side and drift once, and winds the head home before the
  end. Per turn that would send every head home after every sentence, the
  exact twitch `GazeOptions` is written to avoid. With a continuity struct the
  planner opens from where the head is going, keeps the per-scene draws, plans
  in session-absolute frames so a sweep that straddles a turn boundary keeps
  going, and skips the tail settle. The speaker plays to the house; listeners
  react with the usual jitter and turn to the speaker.

A single participant never changes role and never has a stage, so the classic
ad-hoc stream renders exactly the frames it did before.

### `/finish`: one exchange, one animation

As before, the per-turn WAVs are stitched into `<session>.wav` with iXML
provenance and the `AdHocExchange` record is finalized. New for dialogs:

- the exchange record carries `participants` (id, name, audio channel),
  `stage_id`, and a `creature_id`/`creature_name` on every part. The
  top-level `creature_id`/`creature_name` still mean "first participant" so
  existing readers keep decoding, and single-creature records keep their
  pre-#186 shape byte for byte;
- the transcript names who said what, line by line; the title is the cast
  plus the words; provenance lists every lane and speaker, so the MP3 export's
  `ARTIST`/`TRACK_LIST`/`LYRICS` come out multi-speaker for free;
- the turns' tracks are concatenated into one N-track ad-hoc animation over
  the stitched WAV. Each turn's frame count was rounded *up* from its audio,
  so naïve concatenation drifts ahead of the audio by up to a frame per turn.
  `concatTurnTracks` ([TurnConcat.h](../src/server/voice/TurnConcat.h)) sizes
  each part from where its audio actually sits in the stitched file, so lip
  sync stays aligned to the last word. The animation carries `render_seed`,
  `source_render_choices`, `source_stage_id` and `source_stage_updated_at`
  like a dialog job render.

### Endpoints

All on `DialogStreamController` ([DialogStreamController.h](../src/server/ws/controller/DialogStreamController.h)),
contracts in [DialogStreamContracts.h](../src/api/DialogStreamContracts.h).
`traceparent` is honoured by `runEndpoint` exactly as on `/ad-hoc-stream`.

| Endpoint | Body | Response |
|---|---|---|
| `POST /api/v1/animation/dialog-stream/start` | `{creature_ids: [uuid…] (1–8, distinct), stage_id: uuid, resume_playlist?: bool}` | `{session_id, status, message, creature_ids, stage_id}` |
| `POST /api/v1/animation/dialog-stream/turn` | `{session_id, creature_id, text}` | `{session_id, status, turns_received}` |
| `POST /api/v1/animation/dialog-stream/finish` | `{session_id}` | `{session_id, status, message, animation_id, last_turn_animation_id, playback_triggered, exchange_status, parts_rendered, parts_total}` |

Errors mirror `/ad-hoc-stream`: 404 unknown session or stage; 400 non-UUID,
empty or duplicate `creature_ids`, missing `stage_id`, participant not placed
on the stage, creature not in the session, participants on different
universes, mixed frame rates, shared audio channel; 409 session already
finishing, admission limits, or a participant not registered with a universe.
`/ad-hoc-stream/text` against a session with several participants is a 409:
those must say who is speaking. The `/ad-hoc-stream/exchange*` read endpoints
serve dialog sessions unchanged.

### Preserved invariants

- `/ad-hoc-stream` request and response shapes, records, and rendered frames
  are unchanged. The session class is shared, but every #186 branch is gated
  on `participants_.size() > 1`, `dialog`, or a bound stage.
- Turn 1's `interrupt()` still cancels the whole universe (a bystander's idle
  is cancelled once and restarts itself), and later turns only touch the
  participants — identical to `/dialog` autoplay and to the ad-hoc stream.
- Every turn carries every participant's track, so the runner's idle-restart
  check after the chain drains sees a constant creature set.

## Testing

- `tests/server/voice/GazeTrack_test.cpp` — `GazeContinuityTest`: null
  continuity byte-identical; first call primes; no settle at a streamed turn's
  end; next turn opens where the last ended; a sweep straddling a boundary
  keeps going; speaker→listener aims change.
- `tests/server/voice/SpeechTrackBuilder_test.cpp` — `lastBodyFrame`,
  `idleEndOffset`, entry fade blends / is ignored when unset or mis-sized /
  applies in idle mode.
- `tests/server/voice/TurnConcat_test.cpp` — verbatim on exact multiples,
  trims rounded-up parts so 10 × 1.5-frame turns make 15 frames not 20,
  repeats a short part's last frame, matches tracks by creature, rejects bad
  input.
- `tests/model/AdHocExchange_test.cpp` — single-creature records keep their
  shape; dialog round-trip; malformed cast/stage/speaker rejected.
- `tests/api/DialogStreamContracts_test.cpp` and additions to
  `StreamingAdHocContracts_test.cpp` — strict parsing, required stage, cast
  bounds and duplicate detection, stable response shapes, exchange DTO for
  both record generations.
- Live: a Beaky + Mango session on the mainstage, alternating turns; the
  stitched `animation_id` played from `/api/v1/animation/ad-hoc/{id}`; the
  exchange's `audio.mp3` tags; the Honeycomb trace parenting to the world's
  `scene` span.

## Files touched

| File | Change |
|---|---|
| `src/server/voice/GazeTrack.{h,cpp}` | `GazeAxisMotion`, `GazeContinuity`, continuity parameter on `buildGazeTrack` |
| `src/server/voice/SpeechTrackBuilder.{h,cpp}` | `entryFadeFrom`/`entryFadeFrames`; `idleEndOffset`, `lastBodyFrame` |
| `src/server/voice/TurnConcat.{h,cpp}` | new: drift-free per-participant track concatenation |
| `src/server/voice/StreamingAdHocSession.{h,cpp}` | participants, stage binding, `addTurn`, `TurnContinuity`, N-track turns, stitched exchange animation, config-based manager overload |
| `src/model/AdHocExchange.{h,cpp}` | `participants`, `stage_id`, per-part speaker |
| `src/api/StreamingAdHocContracts.h` | exchange DTO carries participants, stage, per-part speaker |
| `src/api/DialogStreamContracts.h` | new: request/response contracts |
| `src/server/ws/controller/DialogStreamController.h` | new: the three endpoints |
| `src/server/ws/App.cpp` | register the controller |
| `CMakeLists.txt` | test sources |
| `docs/ad-hoc-speech.md` | client section |

## Out of scope (follow-ups)

- A settle-home tail after the final turn: heads hand off to the idle loop
  where the last turn left them, as the single-creature stream does today.
- Console UI for multi-participant exchanges (the record stays decodable by
  the current console; it shows the first participant).
- A bystander's idle animation with audio taking the single RTP output lease
  mid-dialog — pre-existing, and the same for a single-creature stream.
