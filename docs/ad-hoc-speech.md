# Ad-Hoc Speech Animations

This document explains how the ad-hoc speech feature works, how it is configured, and how to keep the temporary artifacts tidy on long-running deployments.

## Overview

1. **Client call** – `POST /api/v1/animation/adhoc` with:
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
   - Runs Rhubarb lip sync against the WAV, producing a JSON of mouth cues.
   - Uses `SoundDataProcessor` to inject the mouth values into the chosen speech-loop animation/track.
   - Stores the synthesized animation in the new `adhoc_animations` collection (Mongo TTL + job metadata).
   - Interrupts the cooperative scheduler on the creature’s current universe (mirrors `/api/v1/animation/interrupt` behavior, including optional playlist resume).

3. **Artifacts**
   - A per-job temp directory is created under `${TMPDIR}/creature-adhoc/<job_id>/`.
   - Files follow the pattern `adhoc_<creature>_<timestamp>_<slug>.{wav,mp3,json,txt}` to make manual inspection easy.
   - At server startup we prune directories older than `--adhoc-animation-ttl-hours` (default: 12h), keeping temp artifacts loosely in sync with the Mongo TTL.

## Configuration

| Setting | Purpose | Default / Example |
| --- | --- | --- |
| `speech_loop_animation_ids` (creature JSON) | Animation IDs that can serve as the base motion loop. At least one is required for ad-hoc jobs. | `["speech-loop-beaky-soft", "speech-loop-beaky-big"]` |
| `--adhoc-animation-ttl-hours` or `ADHOC_ANIMATION_TTL_HOURS` | Lifetime for both Mongo `adhoc_animations` documents and temp directories. | `12` |
| Cooperative scheduler | Required. The API rejects requests if the server runs with the legacy scheduler. | `--scheduler cooperative` (default) |

## Monitoring Jobs

The job manager broadcasts:
- `job-progress` – includes `progress` (0.0–1.0) and a text status (speech synthesis, Rhubarb, scheduling, etc.).
- `job-complete` – contains the `result` JSON (`animation_id`, `sound_file`, `universe`, `resume_playlist`, `temp_directory`) and success/failure state.

Use the job ID returned by the REST API to filter messages per request.

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
