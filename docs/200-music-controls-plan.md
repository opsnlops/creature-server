# Music 2.5 + full generation controls (#200)

## Goal

Move dialog background music to ElevenLabs Music 2.5 and expose every generation
control the API offers for our flow, so the console can iterate a take until it
is exactly right instead of re-rolling a bare prompt.

Everything here is additive to the contract in `dialog-background-music-plan.md`.
A console that keeps sending today's `{prompt, duration_extension_ms,
generation_mode}` body keeps working; it just gets `music_v2_5`.

## What ElevenLabs offers (live `openapi.json`, 2026-09-18)

`POST /v1/music/detailed` has two mutually exclusive request shapes:

| Shape | Fields | Length |
|---|---|---|
| **prompt** | `prompt` (≤4100), `music_length_ms`, `generation_mode` (track/loop/ambience — hidden field), `force_instrumental` | server-derived from the dialog take |
| **composition_plan** | `chunks[1..30]`, each a *generation* chunk or an *audio-reference* chunk, plus `seed` | sum of chunk `duration_ms` |

Both accept `model_id` (`music_v2`, `music_v2_5`; v1 deprecated), `finetune_id`
(≤100) + `finetune_strength` (0–2.0, hidden), `store_for_inpainting`.

Generation chunk: `text` (≤6132; optional `[Section]` prefix, `{direction}`
inline), `duration_ms` 3000–120000, `positive_styles[≤50]`, `negative_styles[≤50]`,
`context_adherence` low/medium/high (default high), optional `conditioning_ref`
`{song_id, range{start_ms,end_ms}}` + `condition_strength` low/medium/high/xhigh.

Audio-reference chunk: `{song_id, range}` — re-renders that span of a
previously generated song. This is how "keep 0:00–0:12, redo the rest" works.
Verified live 2026-09-18: the referenced span is strongly correlated with the
source (0.38 vs ~0 for unrelated material) but **not byte-identical** — the
model re-synthesizes it, so don't promise the console a sample-exact splice.
The source must have been generated with `store_for_inpainting: true`.

`POST /v1/music/plan` turns `prompt` + `music_length_ms` (+ optional
`source_composition_plan`) into a plan. **It must be called with an explicit
`model_id`** — the default is `music_v1`, whose plan shape (`MusicPrompt`) is
rejected by v2/v2.5.

`GET /v1/music/finetunes` lists the account's finetunes.

Music 2.5 itself adds no request fields; the 2026-09-14 changelog only widens
`MusicModelID` and raises chunk text to 6132 chars.

Not in scope: streaming, stem separation, upload, video-to-music, `with_timestamps`,
`with_waveform_visual`, `sign_with_c2pa`.

### Why the server validates plan shape itself

`elevenlabs_http::checkResponse` deliberately never forwards an upstream 4xx
body to our clients (it can echo caller text and quota/debug data). A console
that sends a bad plan would only see `ElevenLabs music HTTP 422`. So every
documented limit above is enforced in `api::dialogMusicRequestFromJson` with a
full field path in the message, per `json-codec-conventions.md`.

## HTTP contract

### `POST /api/v1/animation/dialog/music` (existing, widened)

Common fields: `script_id`, `dialog_cache_key`, `dialog_generation_id`,
`model_id?` (`music_v2` | `music_v2_5`, default `music_v2_5`), `finetune_id?`,
`finetune_strength?` (0–2, default 1.0, only with `finetune_id`),
`store_for_inpainting?` (default **true** — every take is a valid audio
reference for the next one).

Exactly one of:

```jsonc
// prompt mode — unchanged behaviour plus force_instrumental
{ "prompt": "…", "duration_extension_ms": 3000, "generation_mode": "track",
  "force_instrumental": true }

// plan mode — the console owns the timeline
{ "seed": 12345,
  "composition_plan": { "chunks": [
    { "song_id": "<prior take song_id>", "range": { "start_ms": 0, "end_ms": 12000 } },
    { "text": "[Bridge] {swelling strings}", "duration_ms": 18000,
      "positive_styles": ["chamber orchestra", "playful"], "negative_styles": ["drums"],
      "context_adherence": "high",
      "conditioning_ref": { "song_id": "…", "range": { "start_ms": 0, "end_ms": 12000 } },
      "condition_strength": "medium" }
  ] } }
```

`duration_extension_ms`, `generation_mode`, `force_instrumental` are prompt-only;
`seed` is plan-only (ElevenLabs rejects it with `prompt`). A chunk is an
audio-reference chunk when it has `song_id`, a generation chunk when it has
`text`; never both. Plan total must be ≥ the dialog take's length and ≤ 600000 ms
(the server derives the take length exactly as before; a plan shorter than the
dialog is a validation error so the BGM always covers the speech).

Job result (`dialog-music` completion) gains:

```jsonc
{ …existing…, "model_id": "music_v2_5", "song_id": "…", "seed": 12345,
  "generation_mode": "track" | null-omitted, "force_instrumental": true,
  "finetune_id": "…", "finetune_strength": 1.0,
  "composition_plan": { "chunks": [ … ] } }   // the plan ElevenLabs actually used
```

`song_id` + `composition_plan` are what the console feeds back into the next
request (audio-reference / conditioning / edit-a-chunk).

### `POST /api/v1/animation/dialog/music/plan` (new, synchronous)

```json
{ "dialog_cache_key": "…", "dialog_generation_id": "…", "prompt": "…",
  "duration_extension_ms": 3000, "model_id": "music_v2_5",
  "source_composition_plan": { "chunks": [ … ] } }
```

Returns `{ "model_id", "music_length_ms", "dialog_duration_ms",
"composition_plan": { "chunks": [ … ] } }`. Sized from the cached take exactly
like a prompt-mode generation, so the plan drops straight into the generate
request. No script lookup: this is a stateless helper over a cached take.

### `GET /api/v1/animation/dialog/music/generated/{id}/recipe` (new)

Returns the same block the job result carries (`model_id`, `song_id`, `seed`,
prompt-mode fields, `composition_plan`, `song_metadata`) read from the cached
candidate's provenance, so the console can reopen a take after a restart and
tweak it. 404 once the candidate has aged out of the cache.

### `GET /api/v1/animation/dialog/music/finetunes` (new)

Proxies `GET /v1/music/finetunes` as `{ "count", "items": [{ "finetune_id",
"name", "model_id", "status", "visibility", "created_by", "tags",
"primary_genre"?, "training_progress" }] }` for a picker.

## Status

Implemented on branch `200-music-controls` → 3.47.0. Everything below is as
built; the "Open decisions" stand as chosen.

## Server changes

1. `voice::MusicClient`
   - Replace `generateInstrumental(apiKey, prompt, lengthMs, mode, …)` with
     `generate(apiKey, const MusicGenerationRequest &, …)` where the request
     struct holds every knob; the JSON body is built from it. Existing tests keep
     covering the multipart parser.
   - Add `generatePlan(...)` → `POST /v1/music/plan`.
   - Add `listFinetunes(...)` → `GET /v1/music/finetunes`.
   - Constants for every documented limit live in `MusicTypes.h` so the
     contract parser and the client agree.
2. `api::DialogMusicRequest` / `DialogContracts.h`
   - Neutral structs `voice::MusicCompositionPlan`, `MusicPlanChunk`
     (one struct; `audioRef` set ⇒ audio-reference chunk), `MusicAudioRange`,
     `MusicGenerationRequest` in `src/server/voice/MusicTypes.h`, with the
     single serializer that feeds both job details and the upstream body.
   - Parser enforces the mutual-exclusion and limit rules above; serializer
     round-trips exactly (the request is stored as job `details` JSON and
     re-parsed by `JobWorker`).
   - New `DialogMusicPlanRequest` / `DialogMusicPlanResult` /
     `DialogMusicRecipe` / `MusicFinetune` contracts.
3. `ws::DialogMusicService::generate` builds the client request from the
   contract, derives the take length once, validates plan coverage, and records
   the new knobs in provenance.
4. `voice::MusicWavProvenance` gains `seed` (optional), `finetuneId`,
   `finetuneStrength`, `storedForInpainting`. Writer + reader (iXML and JSON
   forms) + ID3 (`SoundRenditionService`) updated together with round-trip
   tests — metadata is truth. `requestJson` already carries the full upstream
   body; `prompt` is empty for plan-mode takes.
5. `DialogMusicController`: three new endpoints; `submitDialogMusic` gains span
   attributes for `music.model_id`, `music.request_kind`, `music.chunk_count`,
   `music.seed_present`, `music.finetune_present`.
6. Defaults: `model_id` → `music_v2_5` everywhere (including the plan proxy).

## Tests

- `DialogContracts_test`: prompt/plan mutual exclusion, prompt-only and
  plan-only fields, chunk kinds, every limit, unknown fields, exact round-trip of
  a full plan-mode request, result JSON keys.
- `MusicClient_test`: request-body builder for prompt and plan modes (no
  network), plan-response parsing, finetune-list parsing.
- `IxmlWriter_test` / `IxmlReader_test`: new provenance fields round-trip.
- Live against ElevenLabs (2026-09-18, before deploy): `/v1/music/plan` with
  `model_id` → chunk plan (without it → v1 `sections` shape, confirming the
  gotcha); plan output carries explicit `null` `conditioning_ref` /
  `condition_strength`, which the parser now accepts as absent; plan-mode
  generation with `seed` + `store_for_inpainting` → 200 with `song-id`;
  a second generation audio-referencing that song plus `conditioning_ref` +
  `condition_strength` → 200; prompt mode with `finetune_id` +
  `finetune_strength` + `loop` + `force_instrumental=false` → 200;
  `/v1/music/finetunes` → 45 public finetunes in the documented shape.
- After deploy: prompt-mode take on prod → plan-mode take that
  audio-references it → recipe endpoint → promote → render on channel 17.

## Promotion rules (changed in 3.47.3)

Found on prod: promoting take A made take B un-promotable ("dialog changed")
because the #110 guard compared the candidate's recorded `updated_at` with the
script's, and promotion itself bumps `updated_at` — as does a title or stage
edit. The console had already removed the same rule from its own freshness
check for the same reason (`DialogPreviewPanel.swift`, `DialogMusicCandidate`).

Promote now requires exactly what the console's `matches()` requires: the
candidate's `source_dialog_cache_key` + `source_dialog_generation_id` equal the
script's **accepted voice**. That is the real #136 invariant (music is fitted
to one performance) and it also catches music composed against a non-accepted
preview, which `updated_at` never did. The current-turns cache-key check stays.
Errors: `NoAcceptedVoice`, `StaleDialogRevision`, `MissingCompositionSource`.

Related: generate and plan now read the dialog take from the durable
accepted-take store first (#146), then the ephemeral cache. Reading only the
cache meant any script whose acceptance predated the last cron sweep could not
get music at all (`DialogCache: no generation … on disk`).

## Open decisions (flag, not block)

- `store_for_inpainting` default `true` means ElevenLabs retains every take on
  their side. It is what makes audio-reference work at all; flip to `false` per
  request if a take must not be kept.
- Plan-shorter-than-dialog is rejected rather than allowed. Consistent with
  prompt mode, which always requests at least the dialog length.
