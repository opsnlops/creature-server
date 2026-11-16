//
// CacheMaintenance.cpp - Background prune for an on-disk cache, on a schedule
//

#include "util/CacheMaintenance.h"

#include <thread>

#include <spdlog/spdlog.h>

#include "creature/thread/ThreadName.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::util {

CacheMaintenance::CacheMaintenance(Config config, CachePrunePolicy policy, PruneFn prune)
    : config_(std::move(config)), policy_(std::move(policy)), prune_(std::move(prune)) {

    // The sweep abandons itself between entries once a stop is requested, so a
    // shutdown never waits on a walk of a big or slow cache.
    policy_.abortRequested = [this] { return stop_requested.load(); };
}

void CacheMaintenance::start() {
    info("{} maintenance starting: {}, first pass in {}s, then every {}h", config_.subject, policy_.describe(),
         config_.startupDelay.count(), std::chrono::duration_cast<std::chrono::hours>(config_.interval).count());
    StoppableThread::start();
}

bool CacheMaintenance::sleepUnlessStopping(std::chrono::seconds total) {
    // Sliced rather than one long sleep: a 6 hour sleep would hold up shutdown
    // for as long as it had left to run.
    for (auto slept = std::chrono::seconds(0); slept < total; slept += std::chrono::seconds(1)) {
        if (stop_requested.load()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    return !stop_requested.load();
}

void CacheMaintenance::sweep() {
    // Its own root span, so every automatic run is visible. The absence of
    // this span over 30 days of production data is how the gap this class
    // closes stayed invisible (issue #225) -- and the ad-hoc root had no sweep
    // to be invisible at all until #271.
    auto span = creatures::observability ? creatures::observability->createOperationSpan(config_.spanName) : nullptr;

    if (!prune_) {
        warn("{} maintenance has no prune function; nothing to do", config_.subject);
        return;
    }

    auto result = prune_(policy_, /*dryRun=*/false, span);
    if (!result.isSuccess()) {
        const auto message = result.getError()->getMessage();
        warn("{} maintenance sweep failed: {}", config_.subject, message);
        if (span) {
            span->setError(message);
        }
        return;
    }

    const auto report = result.getValue().value();
    if (span) {
        span->setAttribute("cache.prune.bytes_reclaimed", static_cast<int64_t>(report.bytesReclaimed));
        span->setAttribute("cache.prune.bytes_retained", static_cast<int64_t>(report.bytesRetained));
        span->setAttribute("cache.prune.entries_scanned", static_cast<int64_t>(report.entriesScanned));
        span->setSuccess();
    }
}

void CacheMaintenance::run() {
    setThreadName(config_.threadName);

    // Delay the first pass so startup never contends with a disk walk.
    if (!sleepUnlessStopping(config_.startupDelay)) {
        debug("{} maintenance stopping before its first sweep", config_.subject);
        return;
    }

    while (!stop_requested.load()) {
        sweep();
        if (!sleepUnlessStopping(config_.interval)) {
            break;
        }
    }

    debug("{} maintenance thread stopped", config_.subject);
}

} // namespace creatures::util
