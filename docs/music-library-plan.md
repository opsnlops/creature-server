# Music library: dialog-free generation and saved pieces (#202)

Console counterpart: creature-console #199 (`docs/music-creation-plan.md`,
"Phase 2–4"). Builds on #200 (`docs/200-music-controls-plan.md`).

## Why

April wants to make music in the console's big editor without a dialog, keep
the pieces, refine them over time, and pick an existing piece when authoring a
dialog. Today every music endpoint is bound to an accepted dialog take
(`script_id` + `dialog_cache_key` + `dialog_generation_id`; the take's PCM
length sizes the music), and candidates only live in the LRU cache.

## What was measured first

An audio-reference chunk is a faithful copy of its source. On prod, candidate
`8ae6ad84…` was generated from `22ae492b…` with
`[audio-ref 0–6760 ms, 3000 ms generation conditioned on it (high), seed 11]`;
decoded and compared, the referenced span had sample correlation **0.999 at
0 ms lag** (freshly composed tail with the same seed: 0.72; unrelated: 0.075).
The "strongly correlated, not identical" note in the #200 plan does not
describe this path.

Consequence: **attaching a library piece to a dialog needs no new endpoint.**
The console generates a dialog-bound take whose plan audio-references the
piece's song (extended by a conditioned generation chunk when the dialog is
longer), listens, and promotes it through the existing path — the #136
provenance rule holds because the take *was* composed against that voice.
What the server needs is the library.

## HTTP contract

All JSON per `docs/json-codec-conventions.md`: `rejectUnknownFields` with full
field paths on input, `{count, items}` lists, no `null`s on output.

### `POST /api/v1/music/generate` → 202 `JobCreatedResponse`

The dialog music request without `script_id`, `dialog_cache_key`,
`dialog_generation_id`. Prompt mode adds `music_length_ms` (required,
`kMinMusicLengthMs`–`kMaxMusicLengthMs`) in place of `duration_extension_ms`;
plan mode's length is the plan total as today. Optional `piece_id` (UUID) names
the piece this take refines, recorded in provenance. Same validation, same
2-slot music queue (`tryCreateAndQueueMusicJob`), job type `music`
(`"music"`), result JSON = `dialogMusicGenerationResultToJson` with
`dialog_duration_ms = 0` and `duration_extension_ms = 0`. The worker branch
mirrors the dialog one but skips the script lookup, `loadDialogMusicContext`
and `loadDialogTake`.

Candidates share `MusicGenerationCache`. `saveMusicGeneration` currently
requires `scriptId` to be a UUID; dialog-free candidates store an empty
`scriptId` and the loader accepts empty. `MusicWavProvenance.sourceDialog*`
and `sourceScriptUpdatedAt` stay empty and are documented as "not composed
against a dialog"; the reader must never read empty as "stale".

Aliases so the console has one base path for library work:
`GET /api/v1/music/generated/{filename}` (MP3) and
`GET /api/v1/music/generated/{generationId}/recipe`, backed by the same
service methods as the dialog routes.

### `POST /api/v1/music/generated/{generationId}/save`

```jsonc
{ "title": "Algorithm Divine",          // required for a new piece, ignored with piece_id
  "notes": "",                           // optional
  "piece_id": "…",                       // optional: save as a new version of this piece
  "sections": [ { "text": "[Intro] …", "duration_ms": 8000,
                  "positive_styles": [], "negative_styles": [],
                  "context_adherence": "high" } ] }
```

`sections` are the console's editable sections for this version: generation
chunks without conditioning, validated with the chunk rules, total = the
take's length. They exist because a plan alone loses the content of
audio-referenced chunks; with them a piece reopens fully editable.

Steps, mirroring `DialogMusicService::promote` minus the script gates: load
the candidate (404 once aged out), verify provenance and PCM checksum, write
the permanent WAV as `music/<title-slug>--<id12>.wav` via a new
`util::musicExportBasename(title, generationId)` and
`storage::writeSoundFile(Permanent, …, "music")` (which fires `SoundList` and
marks the index dirty), read back the iXML, then publish the piece. Rollback
the file if publishing fails. Idempotent: saving a generation that is already
a version of the piece returns the piece unchanged.

Works for dialog-bound candidates too ("save to library" in the dialog
editor); `source_dialog` on the version records where it came from.

Response: the `MusicPiece` (201 new, 200 new version).

### `GET /api/v1/music` → `{count, items:[MusicPiece]}`
### `GET /api/v1/music/{id}` → `MusicPiece` (404)
### `PUT /api/v1/music/{id}` `{title?, notes?, current_version_id?}` → `MusicPiece`
### `DELETE /api/v1/music/{id}` → status. Sound files are retained (as promote's are).

### `MusicPiece`

```jsonc
{ "id": "…", "title": "…", "notes": "…",
  "created_at": 1758240000000, "updated_at": 1758240000000,
  "current_version_id": "<generation id>",
  "versions": [
    { "id": "<generation id>", "song_id": "…", "sound_file": "music/….wav",
      "mp3_url": "/api/v1/sound/mp3/….mp3", "duration_ms": 9760,
      "recipe": { …the recipe block… },
      "sections": [ … ],
      "source_dialog": { "script_id": "…", "dialog_cache_key": "…",
                         "dialog_generation_id": "…" },   // only when dialog-bound
      "created_at": 1758240000000 } ] }
```

Versions are append-only; `current_version_id` is which one plays and which
one a refinement references. A version's `song_id` is what the next
refinement references at ElevenLabs.

## Server changes

1. `config.h`: `MUSIC_PIECES_COLLECTION "music_pieces"`.
2. `src/model/MusicPiece.{h,cpp}`: struct + `musicPieceToJson`; version nests
   the recipe via the existing `dialogMusicRecipeToJson`.
3. `src/server/music/{upsert,get,getall}.cpp` mirroring `src/server/script/`:
   `replace_one` upsert, server-managed fields carried forward on update.
4. `storage::publishMusicPiece` / `deleteMusicPiece` through `runPublisher`
   with the new `CacheType::MusicPieceList` → `"music-piece-list"` (edit both
   `CacheInvalidation.h` and `.cpp`).
5. `src/api/MusicContracts.h`: generate request (own to/from JSON pair so the
   job-details round-trip works), save request, piece update request.
6. `JobType::Music` → `"music"`; worker branch; queue admission as for
   dialog music.
7. `MusicController.h` with the routes above; registered in `App.cpp`.
8. `DialogMusicService::generate` split so the ElevenLabs call, provenance
   and cache save are shared by a `MusicService::generate(request, lengthMs)`;
   the dialog path derives the length and validates the take, the library
   path takes it from the request.
9. `Slugify`: `musicExportBasename`.

## Tests

`tests/model/MusicPiece_test.cpp`, `tests/api/MusicContracts_test.cpp`
(missing / wrong type / limits / unknown fields / exact round-trip),
`MusicGenerationCache_test` for the empty-script-id candidate,
`CacheInvalidation_test` for the new kind, `Slugify_test` for the basename.
Live after deploy: generate dialog-free → save → list → refine (plan
referencing the saved version) → save as version → `GET` shows two versions.

## Open decisions (flag, not block)

- Song ids live at ElevenLabs; their retention is not documented. If a song
  is gone, a reference fails at generation time. The console falls back to
  composing from the saved `sections` (no conditioning) and says so.
- Delete keeps the WAVs, matching promote/clear. A "delete files too" flag
  can come later.
