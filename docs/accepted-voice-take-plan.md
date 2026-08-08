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

### "Take generated" wasn't true (found in live testing on 3.40)

The first row of that table described an intention, not the code. Only the
explicit multichannel **export** job ever wrote
`preview-exports/dialog-17ch-<gen>.wav` — and that is exactly the file
`promoteVoiceTake` moves. So accepting a take that had merely been *generated*
404'd on a file that was never written, and the error blamed the 24 h TTL for
a take thirty seconds old.

Two changes make the row true:

1. **Generation writes the ad-hoc export.** The preview job assembles the
   17-channel WAV alongside the metadata it already returns. This is what makes
   "generate four takes, come back tomorrow, pick one" work — the takes are
   real ad-hoc sounds for a day, not just cache entries. Non-fatal: a scene
   whose creatures have no usable `audio_channel` still returns its metadata,
   and accept reports the channel problem precisely if asked to promote it.
2. **Accept assembles it if it's missing, as a job.** Otherwise a take
   generated before this change dead-ends: re-auditioning returns the same
   cached take and never rebuilds the WAV, so there is no way back to an
   acceptable state short of regenerating — which, on eleven_v3, is a
   *different performance*. Assembly reads the cached generation, so it costs
   no ElevenLabs call.

Both go through one `ensureAdHocExport`, which skips a WAV that is already
there. `storage::voiceTakeAdHocPath` is now the single place that knows where
a take's audio lives — export writes it, promote reads it, demote returns it.

**Accept is 200 or 202**, the same shape as `POST /preview/meta`: 200 with the
updated script when the audio is already on disk (the ordinary case, now that
generation writes it), 202 with a `job_id` when it has to be assembled — a
long scene is hundreds of MB and doesn't belong in a request. The
`voice-take-accept` job's completion result is the same script body the 200
returns, so a client that got a 202 ends up with exactly what a 200 would have
given it.

Validation stays synchronous either way: bad UUIDs, a missing script, a stale
`dialog_cache_key`, and a `generation_id` that was never cached are all
answered before any job is promised. The job re-checks the cache key before
stamping, because the turns can change during a long assembly.

Ordering matters on both paths: assembly happens **before** the outgoing take
is demoted, so a failure leaves the previously accepted take whole rather than
half-moved.

## Endpoints

| method | path | body | returns |
|---|---|---|---|
| POST | `/api/v1/animation/dialog/voice/accept` | `{script_id, generation_id, dialog_cache_key}` | 200 script, or 202 `job_id` if the audio needs assembling |
| DELETE | `/api/v1/animation/dialog/script/{scriptId}/voice` | — | 200 script |

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

### Clearing didn't persist (#134, found on 3.40.1)

`DELETE .../voice` demoted the audio, bumped `updated_at`, returned 200 with
`accepted_voice: null` — and left the acceptance in Mongo. `upsertDialogScript`
wrote `$set` with `upsert(true)`, and a `$set` can only add or change: a key
absent from the update document is left untouched. So `updated.erase(...)`
followed by a publish was structurally incapable of removing anything. The 200
was built from the in-memory document, so the endpoint truthfully reported a
state it had never stored.

Worse than a no-op, because the demote *did* run: every clear produced an
acceptance pointing at audio that had moved back to ad-hoc, and once the TTL
swept it the script referenced a file that no longer existed. The accept
endpoint's already-accepted short-circuit then locked the phantom in — the
obvious repair, re-accepting the same take, returned 200 and did nothing.

The upsert now uses `replace_one`. **The document handed to it is the stored
document**, which makes deletion expressible and is what every caller already
assumed. That inverts one responsibility: server-managed fields used to
survive an ordinary script edit by the merge's accident, and now have to be
carried forward deliberately. `updateDialogScript` re-attaches both
`background_music` and `accepted_voice` from the stored script, and
`createDialogScript` strips both — a new script has neither, whatever the body
claims, which also closes a path for a client to forge an acceptance.

Two consequences worth knowing:

- Unknown fields a client stored on a script no longer survive a
  server-initiated write, because `dialogScriptToJson` only emits what the
  model knows. Under `$set` they lingered.
- Every other collection's upsert carried the same trap. All seven are now
  replaces (#135), each with its own merge-reliance dealt with: the fixture
  universe backfill had to move before serialization, and animation render
  provenance is carried forward in the DB layer. `setFixtureUniverse` stays a
  `$set`/`$unset` partial update — it is a genuine field-level write, and it
  could already delete.

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
