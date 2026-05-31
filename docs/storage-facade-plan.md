# Storage facade — implementation plan (issue #11)

## Context

The server has accreted ~5 distinct ways to compute "where do bytes live?" and a manual `scheduleCacheInvalidationEvent` pairing that every file-writing handler has to remember. The pattern's been right twice (it was *forgotten* once on the new dialog handler — caught in review, but the type system gave us nothing to lean on). [Issue #11](https://github.com/opsnlops/creature-server/issues/11) proposes consolidating both concerns behind a `creatures::storage` facade so:

- File path math lives in one place
- Cache invalidation can't be forgotten because the write call fires it
- Future artifact kinds plug in by adding an enum value, not by reinventing both

This refactor is internal — no wire-format changes, no API changes — so it ships as a patch (3.17.0 → **3.17.1**).

## Current landscape (per the exploration)

**5 distinct storage roots:**
| Caller | Root |
|---|---|
| `getAdHocTempRoot()` (JobWorker.cpp:60) | `temp/creature-adhoc/` |
| `getAnimationLipSyncTempRoot()` (JobWorker.cpp:61) | `temp/creature-lipsync/` |
| `DialogCache::dialogCacheRoot()` (DialogCache.cpp:28) | `temp/creature-adhoc/dialog-cache/` |
| `SoundController::generateLipSyncFromUpload` (SoundController.h:323) | `temp/creature-server/lipsync-uploads/` |
| `config->getSoundFileLocation()` | configured permanent root |

**Cache invalidation paired with writes (the manual coupling):**
| Handler | DB write | Invalidations |
|---|---|---|
| `handleAdHocSpeechJob` | `insertAdHocAnimation` | `AdHocAnimationList + AdHocSoundList` |
| `handleAnimationLipSyncJob` | `upsertAnimation` | `Animation` (no new sound) |
| `handleDialogJob` (AdHoc branch) | `insertAdHocAnimation` | `AdHocAnimationList + AdHocSoundList` |
| `handleDialogJob` (Permanent branch) | `upsertAnimation` | `Animation + SoundList` |
| `StreamingAdHocSession::finish` | (caller's) | `AdHocAnimationList + AdHocSoundList` |
| `VoiceController` POST | (service's) | `SoundList` |

**Read side (sound path resolution):** `config->getSoundFileLocation() + "/" + path` joined manually in `CooperativeAnimationScheduler:52`, `LegacyAnimationScheduler:53`, `LocalSdlAudioTransport:55`, `SoundService.cpp:84/309/399`, `SoundController.h:322-329`.

## Approach

### `creatures::storage` facade

Lives at `src/server/storage/Storage.{h,cpp}`. **This is a new source directory** — busts the Phase 1 Docker cache once (per CLAUDE.md). One-time cost.

```cpp
namespace creatures::storage {

// Four buckets, matching the four real lifecycles. Two would collapse
// distinct cleanup stories (JobScratch is per-job and gets cleaned by
// TempDirGuard; AdHoc lives on TTL; GenerationCache is cron-swept;
// Permanent never auto-deletes). Mirrors what the directories already are
// — this is naming the existing reality, not inventing new categories.
enum class Persistence {
    Permanent,        // → config->getSoundFileLocation()
    AdHoc,            // → temp/creature-adhoc/         (TTL-cleaned)
    JobScratch,       // → temp/creature-lipsync/<job>/ (TempDirGuard cleaned)
    GenerationCache,  // → temp/creature-adhoc/dialog-cache/ (cron-swept)
};

struct StoragePath {
    std::filesystem::path absolute;  // for the writer
    std::string forMetadata;         // what to persist on the Animation/Track:
                                     //   absolute  for AdHoc/JobScratch/GenerationCache
                                     //   relative  for Permanent (resolved at read time)
};

// Returns the root directory for a persistence bucket. Ensures parents exist.
Result<std::filesystem::path> root(Persistence);

// Compute a path under the bucket; create intermediate dirs. Does NOT write.
Result<StoragePath> allocateSoundPath(Persistence, std::string filename,
                                      std::optional<std::string> subdir = std::nullopt);

// Write bytes atomically (.tmp + rename); fires the appropriate
// CacheType::*SoundList invalidation on success. Caller can't forget.
Result<StoragePath> writeSoundFile(Persistence, std::string filename,
                                   std::span<const std::uint8_t> bytes,
                                   std::optional<std::string> subdir = std::nullopt);

// Three explicit publish flavors instead of one with a boolean — the call
// site reads as the intent (a new animation? an updated one? an ad-hoc?).
// Each does the DB write AND fires the right invalidations.
Result<creatures::Animation> publishAnimation(const std::string &animationJson,
                                              std::shared_ptr<OperationSpan> = nullptr);
                                              // upsertAnimation + Animation + SoundList

Result<void> publishAdHocAnimation(const creatures::Animation &animation,
                                   std::shared_ptr<OperationSpan> = nullptr);
                                   // insertAdHocAnimation + AdHocAnimationList + AdHocSoundList

Result<creatures::Animation> republishAnimation(const std::string &animationJson,
                                                std::shared_ptr<OperationSpan> = nullptr);
                                                // upsertAnimation + Animation (no sound)

// Read side. Replaces the manual joins. Absolute paths pass through;
// relative paths resolve under Permanent root.
std::filesystem::path resolveSoundPath(const std::string &stored);

} // namespace creatures::storage
```

### Migration order (6 phases, all in one PR)

The issue calls for one cohesive change; doing it in phases sequentially in the same PR keeps the diff reviewable. Each phase compiles + tests independently.

1. **Build the facade** — Storage.h/.cpp + unit tests for path math, atomic write, resolveSoundPath. No call sites touched yet.
2. **Migrate the read side** — replace `config->getSoundFileLocation() + "/" + path` with `creatures::storage::resolveSoundPath(path)` at the 5 sites. Pure 1:1 substitution.
3. **Migrate `handleAdHocSpeechJob`** — `allocateSoundPath(AdHoc, ...)` + `publishAdHocAnimation`. Delete `getAdHocTempRoot()` if no other caller.
4. **Migrate `handleAnimationLipSyncJob`** — `allocateSoundPath(JobScratch, ...)` for per-job temps + `republishAnimation`. Delete `getAnimationLipSyncTempRoot()`.
5. **Migrate `handleDialogJob` (both branches)** — `allocateSoundPath(Permanent|AdHoc, ...)` + `publishAnimation`/`publishAdHocAnimation` per branch.
6. **Migrate small writers + DialogCache** — `StreamingAdHocSession::finish`, `VoiceController` invalidations, `SoundController` lipsync-upload temp. `DialogCache::dialogCacheRoot()` switches to `creatures::storage::root(GenerationCache)`; the atomic write/rename internals stay verbatim — they're load-bearing for the contract on disk.

### What stays out of scope

- **DB-only controllers** (`DialogScript`, `Storyboard`, `Creature`, `Playlist`, `Fixture`) — they don't write files; their `scheduleCacheInvalidationEvent` calls live next to the DB upsert and stay where they are. The facade is *only* for file-coupled mutations.
- **Sound path canonicalization / sanitization** in `SoundController` — that's a security boundary (path-traversal defense). It stays inline; the facade is for path *construction*, not validation.
- **TTL / cron sweep** — DialogCache's existing cron sweep stays. The facade just supplies the root path.

## Files to create

- `src/server/storage/Storage.h` + `Storage.cpp` — the facade
- `tests/server/storage/Storage_test.cpp` — path math, persistence-bucket selection, atomic write durability, resolveSoundPath roundtrip

## Files to modify

- `CMakeLists.txt` — glob `src/server/storage/*`; add the test to `creature-server-test`
- `src/server/jobs/JobWorker.cpp` — three handler migrations + delete the two anon-namespace helpers
- `src/server/voice/DialogCache.cpp` — swap `dialogCacheRoot()` for the facade
- `src/server/voice/StreamingAdHocSession.cpp` — replace `scheduleCacheInvalidationEvent` pair with `publishAdHocAnimation`
- `src/server/ws/service/VoiceService.cpp` — fold invalidation into the write call
- `src/server/ws/service/SoundService.cpp` — `resolveSoundPath` on the read paths
- `src/server/animation/CooperativeAnimationScheduler.cpp` + `LegacyAnimationScheduler.cpp` — `resolveSoundPath`
- `src/server/audio/LocalSdlAudioTransport.cpp` — `resolveSoundPath`
- `src/server/ws/controller/SoundController.h` — `resolveSoundPath` (read path); lipsync-upload temp goes through `allocateSoundPath(JobScratch, ...)`
- `VERSION.txt` — bump to 3.17.1 (internal refactor; no wire-format change)

## Tests

`tests/server/storage/Storage_test.cpp` — these are the load-bearing properties:

1. **Path math** — `allocateSoundPath(P, "foo.wav")` returns expected root for each `P` ∈ {Permanent, AdHoc, JobScratch, GenerationCache}; subdir arg nests correctly; intermediate dirs created on demand.
2. **`forMetadata` semantics** — Permanent returns relative path; everything else returns absolute. (This is the contract that `resolveSoundPath` depends on.)
3. **Atomic write** — `writeSoundFile` writes to `.tmp` then renames; if write fails the `.tmp` is cleaned; if rename fails the target doesn't appear partial.
4. **`resolveSoundPath` roundtrip** — absolute path passes through; relative path joins under `config->getSoundFileLocation()`.
5. **Invalidation pairing** — using a fake invalidation observer (mock for `scheduleCacheInvalidationEvent` or count invocations via test hook), `writeSoundFile(AdHoc, ...)` fires exactly `AdHocSoundList`; `publishAnimation` fires `Animation + SoundList`; etc.

## Verification

1. `ninja creature-server creature-server-test` clean under `-Wshadow -Wall -Wextra -Wpedantic`
2. `./creature-server-test` — full suite still passing (125 + new Storage tests)
3. `clang-format -i` on every touched file
4. Audit: `grep -r scheduleCacheInvalidationEvent src/server/jobs/ src/server/voice/StreamingAdHocSession.cpp src/server/ws/service/` should return empty after migration (DB-only controllers keep their calls).
5. Audit: `grep -r 'getSoundFileLocation()' src/` should be limited to `Storage.cpp` (the canonical caller) plus the security-boundary canonicalize in `SoundController.h`.
6. Live smoke test against prod after deploy:
   - Trigger an ad-hoc speech → confirm Console refreshes the ad-hoc list
   - Render a dialog (permanent) → confirm permanent sound + animation list refresh
   - Run lip-sync on an existing animation → confirm Animation cache refresh
7. Bump VERSION.txt, build debs, deploy.

## Risks

- **Cache invalidation regressions.** The handler-side migrations must preserve the exact set of invalidations per call site. The audit grep in step 4 catches stragglers but the test suite needs to pin "what fires when" so a regression doesn't silently slip.
- **`forMetadata` contract.** Permanent stores relative paths; everything else stores absolute. If a call site stamps the wrong one on `animation.metadata.sound_file`, playback breaks. Migration phase 2 (read side) lands first specifically so this is well-tested before phase 3+ writes.
- **DialogCache atomicity.** The .tmp + rename pattern survives partial failures specifically. The facade's `writeSoundFile` should match those guarantees so DialogCache's migration is just a root swap.
