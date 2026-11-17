# Issue #271 — Get the ad-hoc pipeline out of `/tmp`, and give its root a sweeper that works

**Status:** Implemented
**Issue:** [#271](https://github.com/opsnlops/creature-server/issues/271)

## Problem

The ad-hoc pipeline filled `/tmp` on the mainstage host on 2026-11-15 and took
the show down. Two causes, only one of which is about the directory name.

### A. `/tmp` is the wrong place, and on this host it is RAM

`findmnt /tmp` on `server.local`:

```
TARGET SOURCE FSTYPE OPTIONS
/tmp   tmpfs  tmpfs  rw,nosuid,nodev,nr_inodes=1048576,inode64
```

31 GB of tmpfs, on the 64 GB box that is also holding an 8 GB server process.
Dialog audio is 48 kHz / S16 / 17 channels — **1.63 MB per second of audio** —
so a busy Creature World day of ad-hoc renders spends gigabytes of RAM and
eventually wedges the machine. `/` on the same host has 766 GB free.

Debian Policy §9.1.1 makes FHS 3.0 binding. FHS §3.18 is explicit that programs
must not assume anything in `/tmp` survives between invocations, and recommends
clearing it at boot. An ad-hoc render is not that: it is referenced from
`Animation.metadata.sound_file` in the TTL'd ad-hoc collection for 12 hours, and
must stay servable to the Console and the controllers across a restart.

FHS §5.5 `/var/cache` describes these files almost exactly — generated locally
by time-consuming I/O or calculation, valid across invocations *and* reboots,
expirable by the application or the administrator, and costless to delete
because the application can regenerate them.

The repo already settled this once: #235 moved the Opus packet cache to
`/var/cache/creature-server`, which `scripts/debian/postinst` creates `2775`
`creature-server:creature-server` and `postrm` removes on purge. The ad-hoc
pipeline is the last squatter in `/tmp`.

### B. The sweeper cannot see most of what the pipeline writes

`cleanupAdHocTempDirectory()` (`src/server/main.cpp:183`) is the only thing that
reclaims the ad-hoc root. Four defects, and moving the directory fixes none of
them — it would just make the leak slower and quieter:

1. **It runs once, at startup.** The single call site is `main.cpp:328`. The
   service had been up since 2026-11-10 with `NRestarts=0`, so it had not run in
   five days. On a host that runs ~360 days a year it effectively never runs —
   and on a tmpfs `/tmp` the one moment it *does* run is the one moment the
   directory is guaranteed to be empty anyway.
2. **It only considers directories** (`if (!entry.is_directory()) continue;`).
   `JobWorker.cpp:2244` writes ad-hoc dialog audio as a **loose file** in the
   root — `dialog_<exportName>.wav`, with `wavSubdir` set only for `Permanent` —
   so those files are not candidates at any age. Unbounded.
3. **It ages a whole tree by its parent's mtime.** `preview-exports/`
   (`Storage.cpp:434`) and `dialog-cache/` (the `GenerationCache` root) are
   long-lived top-level directories whose mtime is refreshed by every write, so
   they never fall past the cutoff while their contents accumulate forever.
   Nothing else prunes either, despite `DialogCache.h:30` and
   `DialogCache.cpp:59` both claiming "the existing cron sweep" bounds them.
   There is no cron. There is one call at startup.
4. **There is no byte ceiling.** #225 gave the Opus cache a 32 GiB LRU cap and a
   periodic thread after exactly this lesson. The ad-hoc root never got one.

Net effect: the only reclaimable things were the per-run `<sessionId>/` and
`<jobId>/` directories, and only at a restart.

## Design

### A. One configurable ad-hoc cache root, defaulting under `/var/cache`

`config.h` gains `ADHOC_CACHE_DIR_ENV` / `DEFAULT_ADHOC_CACHE_DIR`
(`/var/cache/creature-server`), plumbed through `Configuration` and
`CommandLine` as `--adhoc-cache-dir` / `adhoc_cache_dir`, exactly like
`audio_cache_dir` from #235.

The default lives in **code**, not only in the shipped `server.json`. That file
is a dpkg conffile, so a host with local edits would keep writing to `/tmp`
after an upgrade — the fix has to land with the package.

`storage::bareRoot()` maps three of the four buckets under it:

| bucket | before | after |
|---|---|---|
| `AdHoc` | `/tmp/creature-adhoc` | `/var/cache/creature-server/adhoc` |
| `GenerationCache` | `/tmp/creature-adhoc/dialog-cache` | `/var/cache/creature-server/dialog-cache` |
| `JobScratch` | `/tmp/creature-lipsync` | `/var/cache/creature-server/scratch` |
| `Permanent` | `/mnt/creatures/sounds` | unchanged |

`JobScratch` is the one bucket `/tmp` could defend — `TempDirGuard` deletes it
at job end and it need not survive anything. It moves anyway, because on this
host `/tmp` is RAM and these are intermediate 48 kHz WAVs, and because a crashed
job's leftovers then fall under the same sweeper. `/var/tmp` (FHS §5.15) would
be the stricter home for scratch-that-is-not-cache; one root with one owner and
one sweeper is worth more here than that distinction.

**Fallback.** If the configured root cannot be created — a macOS dev box, the
test suite, a host where the postinst never ran — `root()` falls back to
`temp_directory_path()`, `warn()`s once with the reason, and reports the
effective root in the startup log. A silent fallback to `/tmp` is how this
outage happened, so it is loud, and the sweeper covers the fallback root too.

### B. Old absolute paths keep resolving

Ad-hoc paths are stored **absolute** in `Animation.metadata.sound_file` — the
documented `StoragePath` convention is relative for `Permanent`, absolute for
everything else. After the move, rows written before the upgrade still point at
`/tmp/creature-adhoc/...`.

`resolveSoundPath()` gains a fallback: if an absolute stored path does not exist
and contains a `creature-adhoc/` segment, retry the remainder under the
configured ad-hoc root. Nothing rewrites the rows; the 12 hour Mongo TTL expires
them on its own.

### C. The prune tiers become shared, not copied

`AudioCache`'s two policy tiers — idle eviction, then LRU-to-fit — are the
debugged part of #225, and are exactly what the ad-hoc root needs. They move to
`src/util/CachePrune.{h,cpp}` as `CachePrunePolicy`, `CachePruneReport`,
`PruneCandidate` and `applyPrunePolicyTiers()`, with both caches building their
own candidate list and supplying their own `evict` callback. `AudioCache` keeps
`using PrunePolicy = util::CachePrunePolicy` so its call sites and tests are
unchanged apart from `forHost` becoming `AudioCache::prunePolicyForHost`.

The policy gains one opt-in field, `minAge`: an entry younger than it is never
evicted by either tier. Unset leaves `AudioCache` behavior exactly as it was.
The ad-hoc store sets it, because the LRU tier would otherwise be free to delete
the directory a streaming session is writing into *right now*.

### D. `AdHocStore::prune()` — a candidate list that matches the tree

`src/server/storage/AdHocStore.{h,cpp}` walks the ad-hoc root and builds one
candidate per independently-expirable thing, which is the defect-2 and defect-3
fix:

- a **loose file** in the root → its own candidate (the `dialog_*.wav` case)
- a **per-run directory** (`<sessionId>/`, `<jobId>/`) → one candidate, dated by
  the **newest** file inside it, so an active session is not judged by the
  timestamp of its first sentence
- `preview-exports/` → **one candidate per file inside**, never the directory
- `dialog-cache/<key>/` → one candidate per key directory, never the root

`*.tmp` leftovers are reclaimed unconditionally, as the Opus cache does.
The two long-lived roots are never themselves candidates, so a sweep can no
longer delete the generation cache wholesale after an idle day — which the old
code would happily have done.

### E. One periodic maintenance thread class, two users

`AudioCacheMaintenance` is generalized into `creatures::util::CacheMaintenance`
(`src/util/CacheMaintenance.{h,cpp}`): same `StoppableThread`, sliced sleep,
startup delay and interval, but constructed with a subject name, a thread name
and a `PruneFn`. `main.cpp` builds two — one for the Opus cache, one for the
ad-hoc root — and the startup-only `cleanupAdHocTempDirectory()` goes away.

Defaults for the ad-hoc sweeper: idle age = the existing
`--adhoc-animation-ttl-hours` (12 h), so one knob still governs the Mongo TTL
and the files; interval 1 h; `minAge` 15 min; cap 16 GiB mainstage / 2 GiB
travel (`ADHOC_CACHE_MAX_BYTES`), chosen against the 31 GB tmpfs it used to
share and the 8 GB travel host.

### F. Publish the retained bytes

The sweep pushes its `bytesRetained` into `SystemCounters` as
`adHocCacheBytes` + `adHocCacheBytesAvailable`, following the `phcOffsetNs`
precedent from #261: a gauge pushed from a background poll, scalar plus validity
flag rather than a nullable number, because the frozen transport contract
records value *types* per machine. Nothing walks a directory on the counter
path; the counter reads the last sweep's result.

This is what lets Honeycomb alert before the root fills instead of after, and
the per-sweep span (`AdHocStore.prune`) carries the same numbers.

## Test plan

- `tests/util/CachePrune_test.cpp` — the shared tiers: idle, LRU-to-fit,
  `minAge` floor, abort between entries.
- `tests/server/storage/AdHocStore_test.cpp` — a fixture tree with a loose
  `dialog_*.wav`, a stale session dir, a fresh session dir, a `preview-exports/`
  with mixed ages and a `dialog-cache/` with mixed ages. Asserts the loose file
  is reclaimed (defect 2), the fresh dir and both long-lived roots survive
  (defect 3), and the cap evicts least-recently-used first.
- `tests/server/storage/Storage_test.cpp` — bucket roots follow the configured
  ad-hoc root; `resolveSoundPath` remaps a legacy `/tmp/creature-adhoc/...`
  path; the fallback engages when the configured root is uncreatable.
- `AudioCache_test.cpp` keeps passing with only the `prunePolicyForHost`
  rename — that is the evidence the tier extraction was behavior-preserving.

## Deployment notes

- `postinst` needs no change: `storage::root()` creates the subdirectories, and
  the `2775` setgid parent from #235 is already group-writable.
- The mainstage unit runs as `root` today (#263), so it can write there
  regardless; the travel unit runs as `april`, who must be in the
  `creature-server` group for the fallback not to engage. Worth checking when
  #263 lands.
- Nothing migrates existing `/tmp/creature-adhoc` content. It ages out of Mongo
  in 12 hours, and a reboot clears the tmpfs.
