//
// AudioCacheMaintenance.cpp - Background prune for the on-disk Opus cache
//

#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "creature/thread/ThreadName.h"
#include "server/audio/AudioCacheMaintenance.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::audio {

AudioCacheMaintenance::AudioCacheMaintenance(std::shared_ptr<creatures::util::AudioCache> cache,
                                             creatures::util::AudioCache::PrunePolicy policy,
                                             std::chrono::seconds startupDelay, std::chrono::seconds interval)
    : cache_(std::move(cache)), policy_(std::move(policy)), startupDelay_(startupDelay), interval_(interval) {

    // The sweep abandons itself between entries once a stop is requested, so a
    // shutdown never waits on a walk of a big or slow cache.
    policy_.abortRequested = [this] { return stop_requested.load(); };
}

void AudioCacheMaintenance::start() {
    info("Audio cache maintenance starting: {}, first pass in {}s, then every {}h", policy_.describe(),
         startupDelay_.count(), std::chrono::duration_cast<std::chrono::hours>(interval_).count());
    StoppableThread::start();
}

bool AudioCacheMaintenance::sleepUnlessStopping(std::chrono::seconds total) {
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

void AudioCacheMaintenance::sweep() {
    // Its own root span, so every automatic run is visible. The absence of
    // this span over 30 days of production data is how the gap this class
    // closes stayed invisible (issue #225).
    auto span = creatures::observability ? creatures::observability->createOperationSpan("AudioCacheMaintenance.sweep")
                                         : nullptr;

    auto result = cache_->prune(policy_, /*dryRun=*/false, span);
    if (!result.isSuccess()) {
        const auto message = result.getError()->getMessage();
        warn("Audio cache maintenance sweep failed: {}", message);
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

void AudioCacheMaintenance::run() {
    setThreadName("AudioCachePrune");

    // Delay the first pass so startup never contends with a disk walk.
    if (!sleepUnlessStopping(startupDelay_)) {
        debug("Audio cache maintenance stopping before its first sweep");
        return;
    }

    while (!stop_requested.load()) {
        sweep();
        if (!sleepUnlessStopping(interval_)) {
            break;
        }
    }

    debug("Audio cache maintenance thread stopped");
}

} // namespace creatures::audio
