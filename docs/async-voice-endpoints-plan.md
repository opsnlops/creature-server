# Async All The Things — Voice/Preview Endpoints Plan (issue #42)

Three endpoints still do ElevenLabs work inside a synchronous HTTP request. Long inputs
outlive the client's 60s timeout while the server keeps working (observed with a 31-turn
dialog preview). Everything long-running moves to the existing job system, which already
carries progress + completion over the WebSocket.

## Endpoints and their new behavior

1. **`POST /animation/dialog/preview/meta`**
   - Cache hit (explicit `generation_id`, or latest take when `regenerate` is false):
     **stays synchronous 200** — it's a fast disk read, and the panel re-auditions often.
   - Generation needed: **202 + `JobCreatedDto`** (new `JobType::DialogPreview`,
     `"dialog-preview"`). JobWorker runs the same loadOrGenerate, broadcasting per-chunk
     progress. Completion `result` = the `DialogPreviewMetaResponseDto` JSON the sync
     path would have returned.

2. **`POST /animation/dialog/preview/multichannel`** — **always 202 + job**
   (`JobType::DialogPreviewExport`, `"dialog-preview-export"`). Even fully-cached
   assembly of a long scene means writing a ~0.5 GB 17-channel WAV; that should never
   ride one HTTP response. The job writes the WAV into the **ad-hoc sound bucket** and
   the completion `result` carries `{ "file_name": ... }` — downloadable through the
   existing `GET /sound/ad-hoc/{filename}` (and, delightfully, shareable through
   `/sound/shareable/{filename}` for free).

3. **`POST /api/v1/voice` (makeSoundFile)** — **202 + job**
   (`JobType::VoiceFile`, `"voice-file"`). Single-voice TTS of long text can also blow
   past 60s. Completion `result` = the existing `CreatureSpeechResponseDto` JSON.

4. **New `GET /api/v1/job/{jobId}`** (small JobController) exposing the JobState —
   status/progress/result. Primarily for the CLI (which has no WebSocket in its REST
   flows) to poll; also handy for debugging.

## Shared service extraction (refs #34)

`DialogPreviewController::loadOrGenerate` + `populateMetaResponse` move into a new
`ws/service/DialogPreviewService` so the controller (sync cache path) and JobWorker
(async generation path) call one implementation. The service takes an optional progress
callback for the worker's broadcasts.

## Versioning / compatibility

3.23.0, stacked on feature/chunked-preview (#41). The preview-meta change is
backward-compatible for cache hits only; clients must understand 202 for the rest —
console + CLI updates ship together (creature-console#11).
