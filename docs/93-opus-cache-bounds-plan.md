# Issue #93 — Bound and deduplicate in-memory Opus cache loads

**Status:** Implemented (including review fixes below)
**Issue:** [#93](https://github.com/opsnlops/creature-server/issues/93)

## Problem

- The disk cache's load limits apply **per channel file**, 17 times
  independently (`AudioCache.cpp:650,700`): `MAX_TOTAL_DATA_SIZE` of 512 MiB ×
  17 channels = the issue's 8.5 GiB worst case, plus ~2 GiB of nested-vector
  overhead — with no aggregate check before allocation. The encode path's own
  cap (`MAX_RTP_PCM_BYTES`, `AudioStreamBuffer.cpp:179`) limits new encodes to
  ~5.5 min, so the two paths disagree wildly.
- Concurrent playbacks of the same immutable source each load a full copy.
  Today's `getFileLoadMutex` (`AudioStreamBuffer.cpp:34`) serializes same-path
  loads but shares nothing. Worse, every cache **hit** deep-copies the whole
  payload (`loadFromCachedAudioData`, `:441`), and a **miss** holds three
  copies at peak (`encodedOpusFrames_` + the copy handed to `saveToCache` +
  the save path's own buffers).
- TOCTOU: `saveToCache(path, …)` re-stats and re-hashes the path
  (`AudioCache.cpp:230,452,470`), untethered from the bytes `MonoWavStream`
  actually read. Swapping the WAV mid-encode publishes version A's packets
  under version B's fingerprint — which then validates as a hit forever.
- Cross-process: temp paths are deterministic (`cachePath + ".tmp"`,
  `AudioCache.cpp:250,278`; fixed `.write_test` at `:810`), so two server
  processes sharing the hostname-scoped cache dir unlink each other's
  in-progress files. No advisory locking exists anywhere in `src/`.

## Design

### A. One duration constant, enforced on both paths

`server/config.h` gains `RTP_MAX_AUDIO_SECONDS` (default **900** — 15 min;
`DialogWav.h` documents realistic scenes as "well under" an hour, and the
current encode cap is only 5.5 min, so this *raises* the encode ceiling while
slashing the cache-read one) with derived `RTP_MAX_FRAMES_PER_CHANNEL` and
`RTP_MAX_ENCODED_TOTAL_BYTES` (payload + per-frame vector overhead × 17
channels ≈ 575 MB at 15 min).

- Cache read: a running aggregate threads through the 17-channel loop; the
  budget check runs **before** each channel's allocation, via an extracted
  free function (unit-testable without allocating anything).
- Encode: the same derived constants replace `MAX_RTP_PCM_BYTES` /
  `MAX_FRAMES_PER_CHANNEL`, so encode and cache-read agree.

### B. Memoization + bounded retention

Beside `getFileLoadMutex` (same weak-map idiom, same per-path lock already
held): a memo of `{SourceFileInfo fingerprint, weak_ptr<AudioStreamBuffer>}`
keyed by canonical path. On load: cheap-validate (size + mtime; full hash only
when the stat changed) and return the existing buffer. The per-path mutex is
already the single-flight — concurrent loads of one file now share one
immutable buffer with zero lifetime changes (every caller already holds a
`shared_ptr` for the whole playback, so `weak_ptr` eviction is automatic and
correct).

Bounded retention on top: a small strong-ref LRU budgeted by a new
`AudioStreamBuffer::approximateBytes()` (computed once at load). Default
budget is hardware-aware (April's sizing): 1 GiB on the main 64 GB server,
128 MiB in travel mode (the 8 GB Pi), overridable via `RTP_AUDIO_MEMO_BYTES`.

Free wins folded in: move instead of copy on cache hit
(`loadFromCachedAudioData`) and pass the encoded array to `saveToCache` by
reference — removing two full-payload copies.

### C. TOCTOU

Capture `SourceFileInfo` **before** opening the WAV; pass it to a new
`saveToCache(path, data, expected, span)` which re-verifies and refuses to
publish (no `.complete` marker) on mismatch. Packets can no longer be
published under a replacement file's fingerprint.

### D. Cross-process safety

- `ScopedFileLock` RAII (`open` + `flock`) on a per-key `.lock` file:
  `LOCK_SH` for `tryLoadFromCache`, `LOCK_EX` for `saveToCache`/`clearCache`,
  nested **inside** the existing process-local `getKeyMutex` (flock is
  per-fd, not per-thread). Corrupt-cache paths release their shared lock
  before invoking `clearCache`, which re-acquires it exclusively — holding it
  across the call would self-deadlock (found by the test suite).
- Temp paths become `.tmp.<pid>.<counter>` (also the writability probe), so
  two processes can never clobber each other's in-progress files.

## Testing

`AudioStreamBuffer.cpp` joins the test target (all its deps are already
linked). New `tests/server/rtp/AudioStreamBuffer_test.cpp` +
`AudioCache` additions:

- aggregate budget function unit tests; an integration test with hand-written
  cache channel files declaring an inflated frame count → rejected before
  allocation
- concurrent loads of one fixture share a single buffer (pointer equality);
  memo invalidation on file change; LRU eviction bounded by the byte budget
  while in-use buffers stay shared via the weak memo
- source replaced between fingerprint capture and save → `InvalidData`, no
  `.complete` marker
- cross-process: a held `LOCK_EX` serializes a concurrent save; temp names
  embed the pid
- WAV fixtures generated on the fly (500 ms, 17-ch) by a minimal in-test
  writer; real Opus encode at that size is well under a second

## Non-goals

- Issue #101's cooperative-cancellation stop token (separate issue; its byte
  budget for *reserved/queued* loads is admission-side and complements these
  content ceilings).
- fd-based reading (`fstat` on the opened stream) — the verify-before-publish
  overload closes the publication hole; an fd plumb through `MonoWavStream`
  is noted as follow-up.

## Files touched

| File | Change |
|---|---|
| `src/server/config.h` | duration constant + derived ceilings |
| `src/util/AudioCache.{h,cpp}` | aggregate budget, verify-before-publish, flock, pid temp paths, move semantics |
| `src/server/rtp/AudioStreamBuffer.{h,cpp}` | memo + LRU, approximateBytes, fingerprint capture, aligned limits, move on hit |
| `tests/server/rtp/AudioStreamBuffer_test.cpp` | new suite |
| `tests/util/AudioCache_test.cpp` | budget/TOCTOU/lock additions |
| `CMakeLists.txt` | test wiring |
| `VERSION.txt` | bump |

## Post-review fixes (2026-08-22)

An adversarial review (four finder angles, 20 findings — recorded on PR #164)
produced these changes:

**Correctness**
- **Memo staleness** (flagged by three angles). The size+mtime fingerprint is
  now a documented *contract* on `BufferMemo`, backed by real levers:
  `invalidateMemo(path)` fires from `storage::writeSoundFile` after the bytes
  land, and `clearMemo()` from the debug sound-list invalidate endpoint. An
  ordinary write moves mtime and self-invalidates; the residual exposure is
  out-of-band *timestamp-preserving* replacement, which the operator lever
  covers.
- **Advisory lock lived inside the directory a clear removes**, so
  `clearCache` unlinked its own inode and a peer could enter concurrently. The
  lock file is now a **sibling** of the cache directory.
- **flock could block forever** while this process held the global encode
  mutex, wedging every cache-miss load. Acquisition is now `LOCK_NB` with a
  bounded retry (5 s), degrading to process-local exclusion with a warning.
- **The release-before-clear trap** (eight copy-pasted `fileLock.reset()`
  sites) is gone: `clearCacheLocked()` is the variant for callers that already
  hold the lock, so the deadlock is now unwritable rather than comment-policed.
- **Zero VBR headroom**: the aggregate budget carries 50% headroom over
  nominal bitrate, and the *save* path enforces the same ceiling — anything
  published is guaranteed loadable, so a dense near-ceiling encode can't
  thrash re-encoding on every play.
- **Orphaned temps**: unique temp names never self-cleaned, so a crash
  mid-save leaked up to 17 files per incident. A save now sweeps abandoned
  `*.tmp.*` for its key under the exclusive lock.
- **Budget latch ordering**: the retention budget is injected explicitly at
  startup (`setMemoRetainBytes`) instead of latching from `config` on first
  use; until then the **smaller travel default** applies, so a load that beats
  configuration can never pin the 64 GB budget on the 8 GB Pi.
- **One-shot loads no longer pin the LRU**: cache prewarms and per-sentence
  streaming speech pass `RetentionIntent::OneShot` — still shared weakly, but
  never charged to the budget, so a long ad-hoc session can't evict warm show
  audio.

**Efficiency**
- The pre-encode SHA-256 moved **outside** `encodingJobMutex` (it throttles
  encoder CPU, not I/O; a show-length hash was serializing every other pending
  miss).
- The publish-time verification is a **stat compare**, not a second full hash
  of the source.
- LRU eviction returns evicted buffers to the caller, which destroys them
  **after** releasing the memo lock (~1.5M deallocations no longer stall
  concurrent memo hits).
- Cache hits reuse the byte total the reader already accumulated instead of
  re-walking every frame vector.
- One canonicalization per load, shared by the load mutex and the memo (they
  can no longer disagree about identity).

**API**
- The legacy `saveToCache(path, CachedAudioData)` overload is **deleted** —
  fingerprint-at-save-time cannot detect a source replaced during the encode,
  and leaving it available invited exactly the bug this issue closes.
- `environmentToUnsignedLongLong` joins the shared env-helper family instead
  of a hand-rolled `getenv`/`strtoull`.

**Deferred to follow-ups** (not this PR): the deterministic `.tmp` idiom still
present in `Storage::atomicWrite` and `DialogCache`, and hoisting one shared
WAV test fixture (the tree now has three hand-rolled RIFF writers).
