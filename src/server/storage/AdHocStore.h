#pragma once

//
// AdHocStore.h — bounds the ad-hoc pipeline's working files (issue #271)
//
// The ad-hoc buckets (`Persistence::AdHoc`, `GenerationCache`, `JobScratch`)
// had exactly one reclaimer: `cleanupAdHocTempDirectory()`, called once at
// startup from main(). It could not do the job, in four separate ways:
//
//  1. It ran only at startup. The main server is up for a month at a time, so
//     on the day the root filled it had not run in five days — and when it did
//     run it was moments after boot, when `/tmp` was a freshly mounted tmpfs
//     and there was nothing there to remove.
//  2. It skipped anything that was not a directory, and the biggest artifact
//     the pipeline writes is a loose `dialog_<name>.wav` in the root.
//  3. It dated a whole tree by the tree's own mtime. `preview-exports/` and
//     `dialog-cache/` are written to constantly, so their mtime never aged
//     past the cutoff and nothing inside them was ever considered.
//  4. It had no size ceiling, so nothing bounded the root between sweeps.
//
// This class is the replacement. It builds one candidate per independently
// expirable thing — which is the fix for (2) and (3) — and hands them to the
// shared policy tiers from `util/CachePrune.h`, which are the same debugged
// tiers the Opus cache has used since #225. `util::CacheMaintenance` runs it
// on a schedule, which is the fix for (1) and (4).
//

#include <cstdint>
#include <memory>
#include <string>

#include "util/CachePrune.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::storage {

class AdHocStore {
  public:
    /**
     * @brief One prune pass over all three ad-hoc buckets.
     *
     * Same signature as `AudioCache::prune`, so `util::CacheMaintenance` can
     * drive either.
     *
     * @param dryRun when true nothing is deleted; the report describes what
     *               would have been.
     */
    static Result<creatures::util::CachePruneReport> prune(const creatures::util::CachePrunePolicy &policy, bool dryRun,
                                                           std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * @brief The policy this host should run with.
     *
     * The idle tier is the **ad-hoc animation TTL** (`ttlHours`,
     * `--adhoc-animation-ttl-hours`), deliberately the same knob that governs
     * the Mongo TTL on the ad-hoc animation and exchange collections: a file
     * whose database record has expired is unreachable, and a record whose
     * file has been reclaimed is unplayable, so the two want one answer.
     *
     * The size cap comes from `ADHOC_CACHE_MAX_BYTES` (see config.h), and the
     * `minAge` floor from `ADHOC_CACHE_MIN_AGE_SECONDS` — without it the size
     * tier would be free to delete the directory a streaming session is
     * filling right now.
     */
    static creatures::util::CachePrunePolicy prunePolicyForHost(bool travelMode, uint32_t ttlHours);
};

} // namespace creatures::storage
