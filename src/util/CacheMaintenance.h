//
// CacheMaintenance.h - Background prune for an on-disk cache, on a schedule
//
#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "util/CachePrune.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/StoppableThread.h"

namespace creatures::util {

/**
 * @brief Runs a cache's prune() on a schedule (issues #225, #271).
 *
 * Began life as `AudioCacheMaintenance`. The Opus cache's prune had existed
 * since #166 and worked, but nothing ever called it: its only caller was the
 * manual debug endpoint, and 30 days of production telemetry contained not one
 * prune span, so the cache grew forever.
 *
 * The ad-hoc root then turned out to have the same disease in a worse form
 * (#271) — its only sweep ran at startup, on a host that stays up for a month
 * at a time — so this class stopped being about audio. It now takes a prune
 * function and a name, and the server runs one of these per cache.
 *
 * Two deliberate properties, unchanged:
 *
 *  - It is its OWN thread, never the 1 ms event loop. A prune stats every file
 *    of every entry; doing that on the event loop would be a show-stopping
 *    stall.
 *  - The first pass is delayed, and every pass runs off the startup critical
 *    path, so the server never waits on a disk walk to come up.
 *
 * Shutdown is prompt: the sleep is sliced so a stop request is noticed within
 * a second, and an in-flight sweep is abandoned between entries rather than
 * holding up exit.
 */
class CacheMaintenance final : public creatures::StoppableThread {
  public:
    /**
     * @brief The cache-specific half: one pass under a policy.
     *
     * Same signature as `AudioCache::prune` and `AdHocStore::prune`, which is
     * not a coincidence — a new cache only has to offer this to be swept.
     */
    using PruneFn =
        std::function<Result<CachePruneReport>(const CachePrunePolicy &, bool dryRun, std::shared_ptr<OperationSpan>)>;

    /**
     * @brief Naming and timing for one sweeper.
     *
     * `subject` appears in log lines ("Audio cache", "Ad-hoc store");
     * `threadName` is what the OS shows (so a stuck sweep is identifiable in
     * `top`); `spanName` is the root span every automatic pass gets, because
     * the absence of exactly such a span is how both of these gaps stayed
     * invisible in production.
     */
    struct Config {
        std::string subject;
        std::string threadName;
        std::string spanName;
        std::chrono::seconds startupDelay{60};
        std::chrono::seconds interval{std::chrono::hours(6)};
    };

    CacheMaintenance(Config config, CachePrunePolicy policy, PruneFn prune);

    ~CacheMaintenance() override = default;

    void start() override;

    /// The policy this host resolved at startup. The manual debug endpoint
    /// previews against the same one, so an operator's dry run shows exactly
    /// what the automatic sweep will do.
    [[nodiscard]] const CachePrunePolicy &policy() const { return policy_; }

  protected:
    void run() override;

  private:
    /// Sleep in slices, returning false as soon as a stop was requested.
    bool sleepUnlessStopping(std::chrono::seconds total);

    /// One pass, with its own root span so each automatic run is visible.
    void sweep();

    Config config_;
    CachePrunePolicy policy_;
    PruneFn prune_;
};

} // namespace creatures::util
