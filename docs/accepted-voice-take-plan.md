# Script-level accepted voice take

Tracking issue: [#131](https://github.com/opsnlops/creature-server/issues/131)

## Context

A dialog script's voice take — the actual generated audio of the birds
speaking — is currently chosen implicitly. Takes live only in the temporary
preview cache, keyed by `sha256(turns)`, and nothing records first-class which
take a render used. The Console reconstructs it by scraping generation-id CSVs
out of sound provenance, which is the root of a family of wrong-take and
confusing-list bugs.

Background music already has the right shape: an explicit, script-level
acceptance that the render reads. This gives voice takes the same treatment,
with acceptance **strict rather than fallback-soft** — un-auditioned audio must
never reach the birds.

This is the third instance of one pattern: the script carries a binding, the
render reads it at render time, an explicit request overrides it. `stage_id`
was #123 (stored) and #128 (actually read). This is the same shape for audio.

---

## A finding the issue didn't have

**Takes are whole-scene. The render reads a per-chunk cache.**

The preview service computes the key over *all* turns and saves the assembled
take under it:

```cpp
probe.cacheKey = computeCacheKey(probe.inputs);   // sha256 of ALL turns
saveGeneration(out.cacheKey, merged);             // the merged take
```

Chunk keys exist only as an internal cache *inside* that assembly. So
`dialog_cache_key = sha256(all turns)` is correct as specified, for every
script — acceptance needs no schema change.

But `handleDialogJob` never looks at whole-scene generations. It loops chunks,
calls `loadGeneration(chunkKey, id)`, and the explicit-id override is gated:

```cpp
const bool useExplicitId = !requestedGenerationId.empty() && chunks.size() == 1;
```

So a multi-chunk script can be accepted and the accepted take can never be
used. Under strict enforcement that's a 400 on every render of a long scene.
Not hypothetical — on prod:

```
 5 turns,  311 chars -> 1 chunk    Test - Beaky Loves Magic
31 turns, 5090 chars -> 3 chunks   MongoDB is Web Scale   <-- would be unrenderable
```

**Decision (April):** take it into account rather than ship the hole. The
render learns to load an accepted take by whole-scene key and use it directly
as one pre-merged chunk. The data already exists in that exact shape — it's
what the preview produced. This also removes the pre-existing limitation that
long scenes can't pin a generation at all.

---

## Model

`DialogScript.accepted_voice`, mirroring `background_music`:

```json
"accepted_voice": {
  "generation_id":    "<uuid>",
  "dialog_cache_key": "<64-char sha256 of the turns it was accepted against>",
  "sound_file":       "dialog/voice/<slug>-<shortid>.wav",
  "accepted_at":      1786000000000
}
```

- **`dialog_cache_key` is the staleness test.** When it no longer equals
  `sha256(current turns)`, the acceptance is *stale* — reported, never
  auto-cleared. Nothing chosen is ever silently un-chosen.
- **`sound_file`** is the promoted, permanent audio, so the Console can play
  the accepted take after the 24 h preview TTL has expired.
- Preserved verbatim across ordinary script edits, exactly as
  `background_music` is.

Filename follows #126: slugified script title plus a short id.

## Storage lifecycle

Acceptance is a **move**, not a copy:

| event | effect |
|---|---|
| take generated | ad-hoc sound, 24 h TTL — re-auditionable for a day |
| **accept** | promote ad-hoc → permanent; audio outlives the TTL |
| **clear** | demote permanent → ad-hoc; TTL restarts (24 h to change your mind) |
| **replace** | accepting B demotes A first |

**Invariant: the sounds directory holds at most one take per script — the
accepted one. Never duplicates, never orphans.**

Note this differs from music promotion, which *copies* (the ad-hoc candidate
survives to TTL). Moving is what keeps the invariant, and it's the same disk
problem as #130 solved by design rather than by cleanup.

## Endpoints

| method | path | body |
|---|---|---|
| POST | `/api/v1/animation/dialog/voice/accept` | `{script_id, generation_id, dialog_cache_key}` |
| DELETE | `/api/v1/animation/dialog/script/{scriptId}/voice` | — |

Accept validates the generation exists for that cache key, demotes any
previously accepted take, promotes the new one, stamps `accepted_voice`, and
returns the canonical script. Clear demotes and unsets. Both mirror the
music promotion / `DELETE .../music` pair.

## Render enforcement

| request | script | result |
|---|---|---|
| explicit `generation_id` | anything | use it — override for CLI and tooling |
| — | fresh acceptance | use the accepted take |
| — | stale acceptance | **400** "the accepted voice take predates the current turns — re-audition and accept" |
| — | no acceptance | **400** "no accepted voice take — audition and accept one first" |

Staleness is `stored dialog_cache_key == computeCacheKey(script's current turns)`.

When an accepted take is used, the render loads it by whole-scene key and
treats it as a single pre-merged chunk, bypassing `chunkTurns` entirely. That
is what makes the strict gate applicable to multi-chunk scripts.

**Inline-turn renders are unaffected** — there's no script to carry an
acceptance, so they keep today's behaviour. The gate applies to `script_id`
renders only.

---

## Files

- `src/model/DialogScript.{h,cpp}` — `AcceptedVoice` struct, DTO, `dialogScriptToJson`
- `src/server/script/helpers.cpp` — parser
- `src/server/ws/service/DialogVoiceService.{h,cpp}` — promote/demote + accept/clear (new; mirrors `DialogMusicService`)
- `src/server/ws/controller/DialogVoiceController.h` — the accept endpoint (new)
- `src/server/ws/controller/DialogScriptController.h` — the clear endpoint
- `src/server/jobs/JobWorker.cpp` — enforcement + whole-scene generation path
- `src/server/ws/App.cpp`, `CMakeLists.txt` — registration

## Verification

- **Round-trip tests in BOTH directions.** The DTO gap has bitten twice —
  #123 (`stage_id`) and `source_render_choices` — both times because only the
  JSON path was covered. Struct→DTO, DTO→struct, and struct→JSON.
- Staleness: fresh key accepted, mutated turns reported stale but not cleared.
- Lifecycle: accept promotes; clear demotes; accepting B demotes A; the
  sounds directory never holds two takes for one script.
- Enforcement: each of the four render cases above.
- **Multi-chunk end to end** — the whole point of the render change. Accept a
  take on a 3-chunk script and render it.
- Guards on the demote path, as with #130: only files under the voice
  subdirectory, containment checked on canonical paths.
