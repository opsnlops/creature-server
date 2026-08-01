# Dialog Background Music

## Goal

Add instrumental ElevenLabs Music generation to the saved-dialog workflow without
making large authoring WAV files cross the network. The server keeps 48 kHz PCM WAV
as the canonical asset, exposes small MP3 renditions for auditioning and sharing,
and places accepted music on channel 17 when it renders a dialog for the normal
RTP/Opus show pipeline.

## Authoring flow

1. The console creates or selects an exact cached dialog preview generation.
2. It submits a music prompt with the saved script id, dialog cache key, and dialog
   generation id.
3. The server returns an ordinary job id. Existing job-progress and job-complete
   WebSocket messages report the long-running ElevenLabs call.
4. The job derives the exact duration from the cached 48 kHz dialog PCM, adds the
   optional client-supplied `duration_extension_ms`, and calls
   `POST /v1/music/detailed?output_format=pcm_48000` with `music_v2` and
   `force_instrumental=true`. This extra requested material compensates for
   ElevenLabs compositions that end with a few seconds of silence. Final show
   rendering continues until whichever ends last: spoken dialog or accepted music.
5. ElevenLabs returns 48 kHz, 16-bit stereo PCM for `pcm_48000`. The server averages
   each left/right sample pair into the single BGM lane without resampling or a
   codec round trip, then stores the authoritative candidate as a temporary mono
   WAV. The job result contains an immutable, static-looking MP3 URL; WAV bytes are
   never sent to the authoring client.
6. Promotion validates the candidate and its embedded provenance, copies it
   atomically to `sounds/dialog/music/` under a descriptive immutable filename,
   and updates the saved DialogScript's background-music reference.
7. A later dialog render loads that permanent WAV and writes it to channel 17 of
   the 17-channel show WAV. The show timeline is the longer of dialog and music;
   creature lanes hold silent/neutral frames through any remaining music tail.

MP3 is a preview/share transport only. It is never accepted as show input.

## HTTP contract

### Generate

`POST /api/v1/animation/dialog/music`

```json
{
  "script_id": "saved-dialog-uuid",
  "dialog_cache_key": "sha256-of-dialog-inputs",
  "dialog_generation_id": "dialog-take-uuid",
  "prompt": "Playful mysterious chamber orchestra under spoken dialogue",
  "duration_extension_ms": 3000,
  "generation_mode": "track"
}
```

The request validates the script UUID, 64-character lowercase cache key, generation
UUID, prompt length, generation mode, and cached dialog take before returning
`202 JobCreatedDto`. `duration_extension_ms` defaults to zero and must be between
0 and 60,000. The total request must remain within ElevenLabs' music-duration
limit. Job type is `dialog-music`.

Successful job result:

```json
{
  "music_generation_id": "music-take-uuid",
  "mp3_url": "/api/v1/animation/dialog/music/generated/music-take-uuid.mp3",
  "duration_seconds": 42.5,
  "dialog_duration_ms": 39500,
  "duration_extension_ms": 3000,
  "requested_music_length_ms": 42500,
  "prompt": "Playful mysterious chamber orchestra under spoken dialogue"
}
```

### Audition a generated take

`GET /api/v1/animation/dialog/music/generated/{generationId}.mp3`

The route reads the cached WAV and uses the shared sound rendition service. It
returns `audio/mpeg`, a descriptive `Content-Disposition`, and
`Cache-Control: public, max-age=31536000, immutable`. Generation ids never change
content; cache cleanup may remove the server copy later without invalidating a
client that already cached it.

### Promote

`POST /api/v1/animation/dialog/music/generated/{generationId}/promote`

The generation sidecar carries the owning script id, so promotion takes no mutable
path or filename from the client. It returns:

```json
{
  "music_generation_id": "music-take-uuid",
  "sound_file": "dialog/music/dialog-title--bgm--prompt-summary--music-take.wav",
  "mp3_url": "/api/v1/sound/mp3/dialog-title--bgm--prompt-summary--music-take.mp3"
}
```

The promoted MP3 is served by the existing sound API and inherits its immutable
cache policy. The permanent WAV remains the show asset.

## Persistence

DialogScript gains an optional `background_music` object containing the permanent
relative `sound_file`, accepted generation id, prompt, and acceptance timestamp.
Old documents parse with no background music. CRUD responses round-trip the optional
object, but normal creates/updates treat it as read-only; only the verified promotion
path may attach or replace it.
Promotion reloads the script before updating it, preserves all existing fields,
and publishes through the storage facade so DialogScript and SoundList cache
invalidations stay paired with successful writes.

Candidate files live under the existing generation-cache lifecycle in a dedicated
`music/` namespace. Each generation has an immutable `.wav` plus bounded `.json`
sidecar. Rejected takes expire with the existing cache cleanup story. Accepted WAVs
are permanent and never regenerated during dialog rendering.

Music generation has a dedicated worker so an upstream composition cannot stall
ordinary dialog and animation jobs. At most two music jobs may be queued or running;
additional requests receive HTTP 429. Candidate storage is pruned to 64 takes and
2 GiB, and generated MP3 renditions are encoded once and cached beside the take.

## WAV provenance

The dialog-only provenance type becomes a generic `WavProvenance`; dialog and music
use one iXML writer, reader, cache serializer, and rendition-tag mapper. Music
provenance stores both searchable fields and the canonical request/response JSON:

- provider, endpoint, model, output format, generation mode, and generation time;
- exact prompt, derived `music_length_ms`, `force_instrumental`, and every explicit
  request option/default sent to ElevenLabs;
- exact source-dialog duration, client-requested duration extension, and final
  ElevenLabs request duration;
- complete canonical request JSON (never the API key);
- complete returned metadata JSON, plus separately indexed composition plan and
  song metadata from the detailed endpoint;
- ElevenLabs song id and request id;
- server music generation id, source dialog/script ids, and SHA-256 of the PCM;
- source channel count and the exact channel transform used to produce the mono
  show lane (`stereo_to_mono_average` for the current ElevenLabs response).

Promotion reads the embedded iXML back before publishing and validates the
generation id plus canonical request. A failed embed or verification leaves both
the permanent sound tree and DialogScript unchanged. The same parsed provenance is
mirrored into MP3 ID3 comments.

ElevenLabs does not guarantee bit-identical regeneration. Prompt-based music cannot
currently use the API's seed field. The provenance therefore preserves the complete
recipe and returned plan for a closely guided future generation, while the accepted
WAV preserves the exact approved performance.

## Shared implementation

- Extract WAV/PCM-to-MP3/Ogg encoding, provenance tag mapping, and rendition result
  metadata from controllers into one `SoundRenditionService`.
- Move the normal sound routes, cached dialog-preview route, and generated-music
  route onto that service.
- Extract the duplicated filename slugifier used by jobs and streaming speech.
- Keep all ElevenLabs HTTP/TLS/error handling in the existing shared HTTP wrapper.
- Add request, service, cache, provenance, interleave, and model round-trip tests.

## Console follow-up

The console work is intentionally not part of this server change:

- [creature-console #65](https://github.com/opsnlops/creature-console/issues/65)
  tracks DTOs, REST calls, and job/WebSocket result integration.
- [creature-console #66](https://github.com/opsnlops/creature-console/issues/66)
  tracks prompt iteration, MP3 auditioning, promotion, accepted-asset display,
  and the existing share flow.

## Validation record

The server implementation was exercised end to end against a blank MongoDB and the
live ElevenLabs Music API. The test generated a 4.4-second instrumental take,
retrieved the generated MP3 twice to verify its immutable cache, promoted the WAV
idempotently, retrieved the promoted asset through the existing sound MP3 route,
and rendered a dialog WAV with 17 channels. Inspection confirmed 48 kHz mono PCM
for the accepted take, standardized iXML plus the complete generation recipe,
matching generated/promoted MP3 content, and non-silent music on channel 17 of the
final RTP source WAV.

A second live test requested a 1,000 ms duration extension for the same 4.4-second
dialog. The server sent `music_length_ms=5400`; ElevenLabs returned a 5.4-second
take, and the resulting job reported all three duration values. Honeycomb's `dev`
environment confirmed those values and all correlation ids on the direct external
call span. It also confirmed that permanent-rendition path resolution remains a
child of the HTTP request trace and exports only a file hash, extension, and store
class—not prompt-derived filenames or filesystem paths.

The final timeline policy was subsequently corrected after console planning caught
that truncating an accepted music tail would make natural fades impossible. The
renderer now uses the later of the spoken-dialog and accepted-music endpoints,
leaves creature audio lanes silent, and emits neutral animation frames while track
17 finishes. Unit coverage verifies both longer-music WAV interleaving and show
timeline selection. Long show WAVs interleave in bounded blocks; accepted-WAV and
iXML chunk extents, music duration, animation frame count, and estimated BSON size
are preflighted before publication and before large accepted WAVs are allocated.
Failed renders remove their partial WAV. Superseded WAVs are retained because the
legacy animation metadata that names them is client-writable and therefore is not
a safe deletion authority. The debug build and all 324
automated tests passed after the final integration, observability, timeline, and
resource-safety changes.
