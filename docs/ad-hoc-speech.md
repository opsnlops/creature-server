# Ad-Hoc Speech Animations

This document explains how the ad-hoc speech feature works, how it is configured, and how to keep the temporary artifacts tidy on long-running deployments.

## Overview

1. **Client call** – `POST /api/v1/animation/ad-hoc` to synthesize + play immediately, or
   `POST /api/v1/animation/ad-hoc/prepare` to build everything but pause playback. Both
   endpoints accept:
   ```json
   {
     "creature_id": "beaky",
     "text": "Hey pals, the show starts in five minutes!",
     "resume_playlist": true
   }
   ```
   The endpoint returns `202 Accepted` with a `JobCreatedDto` (job ID + job type). Watch the existing job-progress/job-complete WebSocket events to show status in the console.

2. **Server job pipeline**
   - Looks up the creature, validates it has `speech_loop_animation_ids`, and picks one at random.
   - Uses `SpeechGenerationManager` (shared with `/api/v1/voice`) to:
     - Generate speech via ElevenLabs (`VoiceClient`).
     - Convert the MP3 to a 17-channel WAV (`AudioConverter`) targeting the creature’s audio channel.
   - Runs Rhubarb lip sync against the WAV, producing a JSON of mouth cues, while pre-warming the 17-channel Opus cache
     in parallel so the first playback already has encoded audio.
   - Uses `SoundDataProcessor` to inject the mouth values into the chosen speech-loop animation/track.
   - Stores the synthesized animation in the new `adhoc_animations` collection (Mongo TTL + job metadata).
   - Interrupts the cooperative scheduler on the creature’s current universe (mirrors `/api/v1/animation/interrupt` behavior, including optional playlist resume).

3. **Artifacts**
   - A per-job temp directory is created under `${TMPDIR}/creature-adhoc/<job_id>/`.
   - Files follow the pattern `adhoc_<creature>_<timestamp>_<slug>.{wav,mp3,json,txt}` to make manual inspection easy.
   - At server startup we prune directories older than `--adhoc-animation-ttl-hours` (default: 12h), keeping temp artifacts loosely in sync with the Mongo TTL.

### Prepare vs Play Later

If you want to line up the speech but wait for the perfect comedic beat, hit the prepare endpoint first:

```bash
curl -X POST http://localhost:8000/api/v1/animation/ad-hoc/prepare \
  -H "Content-Type: application/json" \
  -d '{"creature_id":"mango","text":"Queue me up!", "resume_playlist":false}'
```

The returned job completes exactly like the auto-play variant but sets `auto_play=false` and `playback_triggered=false`
inside the job-complete payload (there is no `universe` because nothing was interrupted yet). When you are ready, call:

```bash
curl -X POST http://localhost:8000/api/v1/animation/ad-hoc/play \
  -H "Content-Type: application/json" \
  -d '{"animation_id":"<UUID from job result>", "resume_playlist":false}'
```

The play endpoint looks up the cached animation, verifies the creature is currently registered, and then interrupts on the
spot. It reuses the same payload as `/api/v1/animation/interrupt`, so `resume_playlist` retains its meaning and defaults
to `true` if omitted.

## Streaming a Dialog Between Several Creatures

`/api/v1/animation/dialog-stream` (3.46.0, issue #186) is the multi-creature form of the streaming
ad-hoc session: the world composes a scene turn by turn and sends each turn the moment it exists; the
server synthesizes it in that creature's voice, lip-syncs it on that creature's channels, and plays it
in arrival order while the other participants cycle their idle loop with their beak shut and look at
the speaker. Turns for different creatures may interleave freely — the server does no turn-taking of
its own. See [186-dialog-stream-plan.md](186-dialog-stream-plan.md) for the design.

Every session is bound to a **stage** (`stage_id` is required) so head aiming works: the speaker
plays to the audience, the listeners turn to the speaker. Every participant must be placed on that
stage and registered on the same universe.

```bash
# 1. Start: who is in the scene, and where they stand.
curl -sS -X POST http://localhost:8000/api/v1/animation/dialog-stream/start \
  -H 'Content-Type: application/json' -H "traceparent: $TRACEPARENT" \
  -d '{"creature_ids":["<beaky uuid>","<mango uuid>"],"stage_id":"<mainstage uuid>","resume_playlist":true}'
# → {"session_id":"…","status":"started","message":"…","creature_ids":[…],"stage_id":"…"}

# 2. One call per sentence, naming the speaker. Playback starts as soon as the first one renders.
curl -sS -X POST http://localhost:8000/api/v1/animation/dialog-stream/turn \
  -H 'Content-Type: application/json' -H "traceparent: $TRACEPARENT" \
  -d '{"session_id":"<session>","creature_id":"<beaky uuid>","text":"April, I think the servos you ordered are here!"}'
curl -sS -X POST http://localhost:8000/api/v1/animation/dialog-stream/turn \
  -H 'Content-Type: application/json' -H "traceparent: $TRACEPARENT" \
  -d '{"session_id":"<session>","creature_id":"<mango uuid>","text":"Or it is more heat sinks. It is always heat sinks."}'
# → {"session_id":"…","status":"ok","turns_received":2}

# 3. Finish: waits for every queued turn to play, then stitches the exchange.
curl -sS -X POST http://localhost:8000/api/v1/animation/dialog-stream/finish \
  -H 'Content-Type: application/json' -H "traceparent: $TRACEPARENT" \
  -d '{"session_id":"<session>"}'
# → {"session_id":"…","status":"completed","message":"…",
#    "animation_id":"<the whole exchange as one ad-hoc animation with every participant's track>",
#    "last_turn_animation_id":"<the last turn's own animation>",
#    "playback_triggered":true,"exchange_status":"ready","parts_rendered":2,"parts_total":2}
```

- `animation_id` is a TTL'd ad-hoc animation (`GET /api/v1/animation/ad-hoc/{id}`) whose tracks are
  the turns concatenated and sized against the stitched WAV, so lip sync stays aligned to the last word.
- The exchange is listed and exportable exactly like a single-creature one
  (`GET /api/v1/animation/ad-hoc-stream/exchanges`, `…/exchange/{session_id}/audio.mp3`). Dialog
  exchanges additionally carry `participants`, `stage_id`, and a `creature_id`/`creature_name` on every
  part; the transcript names who said what; the MP3's `ARTIST` and `LYRICS` list every speaker.
- Errors mirror `/ad-hoc-stream`: `404` unknown session or stage; `400` non-UUID, empty or duplicate
  `creature_ids`, missing `stage_id`, a participant not placed on the stage, a `creature_id` on `/turn`
  that isn't a participant, participants on different universes, or two participants sharing an
  `audio_channel`; `409` session already finishing, admission limits, or a participant whose controller
  isn't registered. `/ad-hoc-stream/text` against a dialog session is a `409` — say who is speaking.
- Trade-off: each turn is a single-voice render, so the voices don't react to each other in tone the way
  the complete-scene `/dialog` render (ElevenLabs Text-to-Dialogue) does. Use this when latency matters
  more than joint conditioning.

## Configuration

| Setting | Purpose | Default / Example |
| --- | --- | --- |
| `speech_loop_animation_ids` (creature JSON) | Animation IDs that can serve as the base motion loop. At least one is required for ad-hoc jobs. | `["speech-loop-beaky-soft", "speech-loop-beaky-big"]` |
| `--adhoc-animation-ttl-hours` or `ADHOC_ANIMATION_TTL_HOURS` | Lifetime for both Mongo `adhoc_animations` documents and temp directories. | `12` |

## Monitoring Jobs

The job manager broadcasts:
- `job-progress` – includes `progress` (0.0–1.0) and a text status (speech synthesis, Rhubarb, scheduling, etc.).
- `job-complete` – contains the `result` JSON (`animation_id`, `sound_file`, `resume_playlist`, `temp_directory`,
  `auto_play`, `playback_triggered`, and `universe` when playback happens) plus the success/failure state.
- `job_type` tells clients which path ran: `ad-hoc-speech` for immediate playback, `ad-hoc-speech-prepare` for staged jobs.

Use the job ID returned by the REST API to filter messages per request.

## Inspecting Generated Assets

Use the REST endpoints below to audit what the server has created recently. All responses are JSON for easy consumption by the iOS/macOS console apps.

- `GET /api/v1/animation/ad-hoc` returns the TTL-backed animation records pulled straight from MongoDB (metadata plus embedded frames). Use this to confirm that an ad-hoc animation was persisted and learn its UUID.
- `GET /api/v1/animation/ad-hoc/{animation_id}` returns the fully hydrated animation (metadata + tracks) just like the primary `/api/v1/animation/{id}` endpoint, but scoped to the TTL collection.
- `GET /api/v1/sound/ad-hoc` walks those same records and surfaces any on-disk WAV artifacts. Each item wraps the familiar sound JSON shape plus extra metadata so existing clients can reuse their parsers:

```json
{
  "animation_id": "70E7861F-D515-4091-9D0A-F7A57952F927",
  "created_at": "2025-10-26T04:12:31Z",
  "sound_file": "/tmp/creature-adhoc/F49100C5-C53D-4D67-A7C6-205A930A7DEA/adhoc_mango_20251025211226_hey-everyone-mango-is-live-on-stage.wav",
  "sound": {
    "file_name": "adhoc_mango_20251025211226_hey-everyone-mango-is-live-on-stage.wav",
    "size": 4220658,
    "transcript": "adhoc_mango_20251025211226_hey-everyone-mango-is-live-on-stage.txt",
    "lipsync": "adhoc_mango_20251025211226_hey-everyone-mango-is-live-on-stage.json"
  }
}
```

### Downloading Ad-Hoc Audio

To stream or download the synthesized WAV, call `GET /api/v1/sound/ad-hoc/{filename}` using the basename from the list response. The controller validates/sanitizes names, ensures the file still resides under `${TMPDIR}/creature-adhoc`, and then serves it with the same headers as the standard `/api/v1/sound/{filename}` download (plus a `Content-Disposition: attachment` hint). The helper files that contain the transcript (`.txt`) and Rhubarb output (`.json`) sit next to the WAV in the same temp directory and follow the same naming convention if you need to retrieve them manually.

Clients should also subscribe to the existing WebSocket cache invalidation stream. Whenever a new ad-hoc job finishes, the server now emits `cache_type` values `ad-hoc-animation-list` and `ad-hoc-sound-list`. Treat those exactly like the legacy `animation`/`sound-list` invalidations and re-fetch `/api/v1/animation/ad-hoc` or `/api/v1/sound/ad-hoc` when observed.

## Housekeeping During Long Uptime

The server deletes stale temp directories at boot, but if the host runs for days/weeks at a time you may want an external cleanup job. You can either add a cron entry or, preferably, a systemd timer. Replace `12` with whatever TTL you configured on the server.

### Systemd Service + Timer

`/etc/systemd/system/creature-adhoc-cleanup.service`
```ini
[Unit]
Description=Remove expired Creature Server ad-hoc speech artifacts

[Service]
Type=oneshot
Environment=TTL_HOURS=12
ExecStart=/usr/bin/env bash -c 'find /tmp/creature-adhoc -mindepth 1 -maxdepth 1 -type d -mmin +$((TTL_HOURS*60)) -print -exec rm -rf {} +'
```

`/etc/systemd/system/creature-adhoc-cleanup.timer`
```ini
[Unit]
Description=Run ad-hoc speech cleanup every hour

[Timer]
OnCalendar=hourly
Persistent=true
Unit=creature-adhoc-cleanup.service

[Install]
WantedBy=timers.target
```

Enable the timer:
```bash
sudo systemctl daemon-reload
sudo systemctl enable --now creature-adhoc-cleanup.timer
```

### Cron Alternative

If systemd timers are not available, add the following to root’s crontab:
```
0 * * * * find /tmp/creature-adhoc -mindepth 1 -maxdepth 1 -type d -mmin +$((12*60)) -print -exec rm -rf {} +
```

Keep the TTL value in the cron entry synchronized with the server’s `--adhoc-animation-ttl-hours` flag.
