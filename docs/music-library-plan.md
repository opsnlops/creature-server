# Music library: dialog-free pieces, versions, and refinement (#202)

Console counterpart: creature-console #199 (`docs/music-creation-plan.md`).
Builds on #200 (`docs/200-music-controls-plan.md`).

## Why

April wants what the native ElevenLabs Music editor gives her: a piece she can
keep coming back to, edit section by section (include/exclude style chips,
lyrics/prompt text, durations), type an instruction like *"add smooth synth
pads and some ambient atmosphere"*, and get a **new variation that keeps the
sections she didn't touch** — instead of throwing the whole thing out. Then
pick a piece when authoring a dialog.

Today every music endpoint is bound to an accepted dialog take, candidates live
only in the LRU cache, and "refine" means re-sending a whole plan by hand.

## What was measured (corrected 2026-09-19)

An audio-reference chunk keeps the **music** of the referenced span but
**re-synthesizes the waveform**. Five trials (direct against ElevenLabs and
through the server on prod, `#200` comment thread): sample correlation with
the source 0.01–0.50, envelope correlation 0.88–0.96; two separate references
to the *same* span of the *same* song correlate 0.01 with each other; unrelated
audio ≈ 0. ElevenLabs' "inserts a slice unchanged" holds at the model's latent
level, not the sample level. An earlier console-side 0.999 could not be
reproduced and has the signature of comparing a decode with itself.

Consequences:

- **Refinement works exactly like the native editor**, because it *is* the
  native mechanism: kept sections are audio-referenced from the current
  version's `song_id`, changed sections are regenerated (optionally
  conditioned on the old audio). They sound the same; they are not the same
  bytes. Nothing in this design depends on byte fidelity.
- **Attaching a piece to a dialog** is a dialog-bound *variation* of the piece
  (sections audio-referenced, sized to the dialog), promoted through the
  existing path so the #136 provenance rule holds. Not a byte copy of the saved
  WAV — same as the native editor would do. No new attach endpoint.

## Model

A **piece** is a title + notes + an append-only list of **versions**. Every
version is one generation (candidate promoted to the library) and carries:

- `sections` — the *editable* form: generation chunks only (text, duration,
  positive/negative styles, adherence), no audio-refs. This is what the console
  shows as chips and what feeds the next refinement. It exists because the
  plan actually sent may contain audio-ref chunks, and those lose the section's
  content.
- `recipe` — the #200 recipe block (model, seed, song_id, the plan actually
  sent, …), read from the WAV's provenance.
- `song_id` — what the *next* refinement audio-references at ElevenLabs.
- `sound_file` (permanent, `music/<title-slug>--<id12>.wav`), `mp3_url`,
  `duration_ms`, `created_at`, optional `source_dialog`.

`current_version_id` is the one that plays and that refinements start from.

## HTTP contract

All JSON per `docs/json-codec-conventions.md`.

### `POST /api/v1/music/generate` → 202 `JobCreatedResponse`

The dialog music request without `script_id` / `dialog_cache_key` /
`dialog_generation_id`, in one of three modes:

| Mode | Fields | Length |
|---|---|---|
| prompt | `prompt`, `music_length_ms` (required, 3000–600000), `generation_mode`, `force_instrumental` | `music_length_ms` |
| plan | `composition_plan`, `seed` (as #200) | plan total |
| **sections** | `sections[]`, `base_version_id?`, `keep?: [indices]`, `seed?`, `condition_strength?` | sections total |

Common: `model_id`, `finetune_id` + `finetune_strength`, `store_for_inpainting`
(default true), `piece_id?` (the piece this candidate refines; recorded in
provenance and used to resolve `base_version_id`).

**Sections mode is the refinement builder**, and it lives on the server so
every client gets the same debugged behaviour:

1. `sections` is the full editable plan for the new version.
2. If `base_version_id` is given (must belong to `piece_id`), each section in
   `keep` must be *unchanged* from the base version's section at the same
   index (same text, styles, adherence **and duration**); the server turns it
   into an audio-ref chunk `{song_id: base.song_id, range: base section's
   cumulative [start, end)}`. Everything else becomes a generation chunk; when
   the base has a section at that index, it gets `conditioning_ref` on the
   base's span (capped at 30 s, ElevenLabs' limit) with `condition_strength`
   (default `medium`) so the new material stays in character.
3. If a `keep` index is not actually unchanged, the request is rejected with
   the index and what differs — the console must not silently regenerate
   something the user thinks is kept.
4. The candidate's provenance records `sections` (`sectionsJson`) and
   `baseVersionId`, so `save` needs nothing re-sent.

Job type `music`, same 2-slot queue. Result JSON = the #200 result with
`dialog_duration_ms` = `duration_extension_ms` = 0 plus `sections` and
`base_version_id` when present.

Candidates share `MusicGenerationCache`; dialog-free ones store an empty
`scriptId` (loader relaxed). `sourceDialog*` / `sourceScriptUpdatedAt` stay
empty and mean "not composed against a dialog", never "stale".

Aliases: `GET /api/v1/music/generated/{filename}` (MP3) and
`…/generated/{generationId}/recipe`, same service methods as the dialog routes.

### `POST /api/v1/music/{id}/refine` → 200 (synchronous, no audio)

The instruction box. `{ "instruction": "…", "version_id"?: "…" }` →

```jsonc
{ "base_version_id": "…",
  "sections": [ …proposed editable sections… ],
  "changed": [1, 4],                      // indices whose content differs from the base
  "kept":    [0, 2, 3, 5] }
```

Server: `POST /v1/music/plan` with the base version's `sections` as
`source_composition_plan`, `prompt` = instruction, `music_length_ms` = base
length, `model_id` = base model; normalise the returned plan to editable
sections; diff against the base by index (text/styles/adherence/duration).
The console shows the proposal, lets April tweak chips, then submits
`generate` in sections mode with `keep = kept`. Two steps on purpose: the
native editor also shows "Song composition has changed" before you press
Generate, and generation costs credits.

`POST /api/v1/music/plan` (dialog-free draft from a prompt + `music_length_ms`
+ optional `source_composition_plan`) is the same proxy as the dialog one
without the take lookup — useful for a brand-new piece.

### `POST /api/v1/music/generated/{generationId}/save`

```jsonc
{ "title": "Algorithm Divine",   // required for a new piece, ignored with piece_id
  "notes": "",                    // optional
  "piece_id": "…",                // optional: new version of this piece
  "sections": [ … ] }             // optional when the candidate carries them
```

Mirrors `DialogMusicService::promote` minus the script gates: load candidate
(404 once aged out), verify provenance + PCM checksum, write
`music/<title-slug>--<id12>.wav` via `util::musicExportBasename` and
`storage::writeSoundFile(Permanent, …, "music")` (fires `SoundList`), read the
iXML back, publish the piece; roll the file back if publishing fails.
Idempotent for a generation that is already a version. Works for dialog-bound
candidates ("save to library"), recording `source_dialog`. Saving sets
`current_version_id` to the new version. Response: `MusicPiece` (201 new
piece, 200 new version).

### `GET /api/v1/music` → `{count, items:[MusicPiece]}`
### `GET /api/v1/music/{id}` → `MusicPiece` (404)
### `PUT /api/v1/music/{id}` `{title?, notes?, current_version_id?}` → `MusicPiece`
### `DELETE /api/v1/music/{id}` → status; WAVs retained (as promote's are).

### `MusicPiece`

```jsonc
{ "id": "…", "title": "…", "notes": "…",
  "created_at": 1758240000000, "updated_at": 1758240000000,
  "current_version_id": "<generation id>",
  "versions": [
    { "id": "<generation id>", "song_id": "…", "sound_file": "music/….wav",
      "mp3_url": "/api/v1/sound/mp3/….mp3", "duration_ms": 9760,
      "recipe": { …#200 recipe block… },
      "sections": [ … ],
      "base_version_id": "…",             // when refined from another version
      "source_dialog": { "script_id", "dialog_cache_key", "dialog_generation_id" }, // dialog-bound only
      "created_at": 1758240000000 } ] }
```

## Server changes

1. `config.h`: `MUSIC_PIECES_COLLECTION "music_pieces"`.
2. `src/model/MusicPiece.{h,cpp}`: structs, `musicPieceToJson`, strict
   `musicPieceFromJson` (persistence normalisation, trusted).
3. `src/server/music/{get,getall,upsert,delete}.cpp` mirroring
   `src/server/script/` (`replace_one`, server-managed fields carried forward).
   New `src/server/music/` dir → one `CMakeLists.txt` glob line (Phase 1 cache
   bust, once).
4. `storage::publishMusicPiece` / `deleteMusicPiece` via `runPublisher` with
   `CacheType::MusicPieceList` → `"music-piece-list"` (`.h` + `.cpp`).
5. `src/api/MusicContracts.h`: generate request (own to/from JSON pair — job
   details round-trip), save / update / refine requests, refine + piece
   responses. Section parsing reuses `detail::musicPlanChunkFromJson` with
   audio-refs and conditioning forbidden.
6. `voice::MusicTypes.h`: `MusicSection` = generation chunk without
   conditioning; `buildSectionsPlan(sections, base, keep, strength)` — the
   builder, pure and unit-tested. `MusicWavProvenance` gains `sectionsJson`,
   `pieceId`, `baseVersionId` (writer/reader/ID3 + round-trip tests).
7. `DialogMusicService::generate` split: the ElevenLabs call → provenance →
   cache-save core moves to `MusicService::compose(request, provenanceSeed,
   span)`; the dialog path keeps the take lookup and length rule, the library
   path takes length from the request. Generalise, don't copy.
8. `JobType::Music` → `"music"`; worker branch; `tryCreateAndQueueMusicJob`
   takes the job type.
9. `MusicController.h` with the routes; registered in `App.cpp`.
10. `Slugify`: `musicExportBasename(title, generationId)`.

## Phases

- **Phase 1 (this PR):** generate (all three modes incl. the sections builder),
  candidate aliases, save, pieces CRUD, `music-piece-list`, provenance fields.
  This alone gives "edit chips / regenerate a section / keep the rest".
- **Phase 2:** `/refine` (instruction → proposed sections + diff) and the
  dialog-free `/plan`. Small once Phase 1's normaliser exists.
- **Phase 3 (console-led):** dialog-bound generate accepts sections mode +
  `piece_id`, so "use this piece under this dialog" is one request.

## Tests

`MusicPiece_test`, `MusicContracts_test` (missing / wrong type / limits /
unknown fields / exact round-trip / keep-index-not-unchanged),
`MusicTypes_test` for the builder (ranges from cumulative durations,
conditioning cap at 30 s, kept-vs-changed), `MusicGenerationCache_test` for
the empty-script-id candidate, `CacheInvalidation_test`, `Slugify_test`.
Live after deploy: generate → save → refine → generate (sections, keep) → save
as version → `GET` shows two versions with the second's plan containing
audio-refs to the first's `song_id`.

## Open decisions (flag, not block)

- Song-id retention at ElevenLabs is undocumented. If a reference fails at
  generation time the job fails with the upstream error; the console can
  regenerate from `sections` without `base_version_id`.
- Delete keeps WAVs, matching promote/clear.
- A version's `sections` total must equal its audio length; `save` rejects a
  mismatch rather than storing sections that don't describe the audio.
