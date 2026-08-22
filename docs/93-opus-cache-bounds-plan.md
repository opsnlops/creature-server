# Issue #93 — Bound and deduplicate in-memory Opus cache loads

**Status:** Implemented
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
