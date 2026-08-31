# Issue #94 — Index sound basename lookup and tighten /sound/play validation

**Status:** Draft — implementing
**Issue:** [#94](https://github.com/opsnlops/creature-server/issues/94)

## Problem

- `resolveSoundInRoot` walks the whole sound tree with
  `fs::recursive_directory_iterator` on every basename miss, on the request
  thread (`SoundPathResolver.cpp:27`) — including `.opus_cache/` and the
  dialog generation cache. A LAN client sending distinct nonexistent names
  forces repeated full-tree scans.
- Duplicate basenames resolve by iteration order. They exist in practice:
  every streaming speech session writes `s1.wav`, `s2.wav`, … under its own
  ad-hoc session dir.
- `/sound/play` validation gaps: C1 control bytes (0x80–0x9F) pass
  `isSafeFilename`; the raw client string is logged and stamped on spans
  before validation; the extension check is mode-independent (RTP accepts
  `.mp3`/`.flac` and then fails deep in a worker thread) and rejects `.wave`
  which the local decoder supports.
- `resolveSoundPath` swallows **every** `HttpError` from the permanent store
  to fall through to ad-hoc — so a 409 raised there would silently become a
  404. The status must be carried, not thrown-and-swallowed.

## Design

### `SoundStoreIndex` (in `SoundPathResolver.{h,cpp}` — already the dependency-free, tested seam)

```cpp
class SoundStoreIndex {
    enum class Status { Found, NotFound, Ambiguous };
    struct Entry { canonicalPath, size, mtime,
                   sampleRate, channels, rtpPlayable };  // #55 tie-in, populated now
    struct Lookup { Status; optional<Entry>; vector<string> candidates; };
    explicit SoundStoreIndex(fs::path root);   // canonicalizes once
    Lookup find(const std::string &basename);  // never walks when warm
    void markDirty();                          // O(1), any thread
    void rebuildNow();                         // tests + debug endpoint
};
```

- `shared_mutex` over `unordered_map<string, vector<Entry>>` — the vector *is*
  the ambiguity representation. `atomic<bool> dirty_` + a rebuild mutex for
  single-flight: invalidation marks dirty (cheap); the first reader after that
  pays for one bounded walk while others wait. Misses never walk once warm.
- Walk skips `.opus_cache`, `dialog-cache`, and dot-directories; same
  extension set as the sound list.
- **Self-healing** (out-of-band writers are a supported workflow —
  `modernize-sounds.py`, streaming sessions, or the voice client): a `Found` hit
  is stat-revalidated; a vanished/changed file drops that entry and falls back
  to a single-file probe (`root/basename`), never a full walk.
- Entries record `sampleRate`/`channels`/`rtpPlayable` from one cheap
  `MonoWavStream` header parse at index time — the format/health metadata
  issue #55 wants, designed in now, exposed later.

### Wiring

- Two globals in `main.cpp` beside `audioCache`: `permanentSoundIndex`
  (`getSoundFileLocation()`) and `adHocSoundIndex` (the storage facade's
  ad-hoc root).
- **Invalidation pairing lives in `Storage.cpp`**, not the event loop: a small
  helper wraps `scheduleCacheInvalidationEvent` and marks the matching index
  dirty, used by `writeSoundFile`, `broadcastCacheInvalidation`, and the
  direct schedule sites (promote/demote, superseded-dialog delete, lipsync).
  `CacheInvalidateEvent` (1 ms event-loop thread) is untouched.
- Debug endpoints (`cache-invalidate/sound-list`, `…/ad-hoc-sound-list`) also
  call `rebuildNow()` — the operator repair button.
- `resolveSoundInRoot` remains as a thin wrapper for the one JobWorker caller;
  `SoundService`'s resolvers switch to the index and return a status-carrying
  result so `Ambiguous` propagates as a deterministic **409** listing the
  candidate paths (permanent-store 409/400 no longer falls through to ad-hoc;
  only NotFound does).

### `/sound/play` validation order

1. null/empty (exists) → 400
2. hardened `isSafeFilename`: adds C1 (0x80–0x9F) rejection to the existing
   255-byte cap, C0/DEL, and path-component checks → 400
3. all logging/span attributes use a bounded, escaped
   `sanitizedForLogging(name)` — including the controller's span stamp
4. mode-aware extensions (mode hoisted above the check): RTP → `{.wav}`;
   local/travel → `{.wav, .wave, .mp3, .flac}` → 400
5. index lookup → 404 / 409 / play

## Testing

Extend `tests/server/audio/SoundPathResolver_test.cpp` (real temp-dir fixture
already exists; fixtures upgraded to real WAV headers via
`writePcmToMultichannelWav` where format metadata matters):

- warm lookups hit without walking (probe: delete the tree after build; find
  still answers from the index, self-heal notices on revalidation)
- duplicate basenames → `Ambiguous` with both candidates, deterministic order
- markDirty → next find rebuilds once (single-flight under concurrent readers)
- self-heal: entry vanishes → dropped + single-file probe; new file at root
  found by probe without a rebuild
- skip-dirs excluded; extension filter enforced
- validation unit tests for the hardened filename rules and log sanitizer
- the old `TopLevelTakesPrecedenceOverSubdir` test is re-scoped: that case is
  now `Ambiguous` by design

`SoundService` itself stays untestable (oatpp-coupled); the extracted index +
validators carry the coverage, per the established pattern.

## Files touched

| File | Change |
|---|---|
| `src/server/audio/SoundPathResolver.{h,cpp}` | `SoundStoreIndex`; wrapper kept |
| `src/server/ws/service/SoundService.{h,cpp}` | status-carrying resolution, 409, validation order, sanitized logging, mode-aware extensions |
| `src/server/ws/controller/SoundController.h` | sanitized span attribute |
| `src/server/storage/Storage.cpp` | invalidation↔dirty pairing helper |
| `src/server/ws/controller/DebugController.h` | rebuildNow on the two endpoints |
| `src/server/main.cpp` | index globals |
| `tests/server/audio/SoundPathResolver_test.cpp` | index + validation suites |
| `VERSION.txt` | bump |
