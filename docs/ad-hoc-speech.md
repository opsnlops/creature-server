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
     - Generate speech via ElevenLabs (`CreatureVoices`).
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

## Configuration

| Setting | Purpose | Default / Example |
| --- | --- | --- |
| `speech_loop_animation_ids` (creature JSON) | Animation IDs that can serve as the base motion loop. At least one is required for ad-hoc jobs. | `["speech-loop-beaky-soft", "speech-loop-beaky-big"]` |
| `--adhoc-animation-ttl-hours` or `ADHOC_ANIMATION_TTL_HOURS` | Lifetime for both Mongo `adhoc_animations` documents and temp directories. | `12` |
| Cooperative scheduler | Required. The API rejects requests if the server runs with the legacy scheduler. | `--scheduler cooperative` (default) |

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
- `GET /api/v1/sound/ad-hoc` walks those same records and surfaces any on-disk WAV artifacts. Each item wraps the familiar `SoundDto` plus extra metadata so existing clients can reuse their parsers:

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
