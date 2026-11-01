//
// AudioCacheMaintenance.h - Background prune for the on-disk Opus cache
//
#pragma once

#include <chrono>
#include <memory>

#include "util/AudioCache.h"
#include "util/StoppableThread.h"

namespace creatures::audio {

/**
 * @brief Runs AudioCache::prune() on a schedule (issue #225).
 *
 * The prune has existed since #166 and worked, but nothing ever called it: its
 * only caller was the manual debug endpoint, and 30 days of production
 * telemetry contained not one prune span. The cache therefore grew forever,
 * inside the permanent sound root, 17 Opus files per sound.
 *
 * Two deliberate properties:
 *
 *  - It is its OWN thread, never the 1 ms event loop. A prune stats every
 *    channel file of every entry; doing that on the event loop would be a
 *    show-stopping stall.
 *  - The first pass is delayed, and every pass runs off the startup critical
 *    path, so the server never waits on a disk walk to come up.
 *
 * Shutdown is prompt: the sleep is sliced so a stop request is noticed within
 * a second, and an in-flight sweep is abandoned between entries rather than
 * holding up exit. Nothing is left inconsistent by that — a half-removed entry
 * has no completion marker, so it reads as a miss and the next sweep reclaims
 * it.
 */
class AudioCacheMaintenance final : public creatures::StoppableThread {
  public:
    AudioCacheMaintenance(std::shared_ptr<creatures::util::AudioCache> cache,
                          creatures::util::AudioCache::PrunePolicy policy, std::chrono::seconds startupDelay,
                          std::chrono::seconds interval);

    ~AudioCacheMaintenance() override = default;

    void start() override;

    /// The policy this host resolved at startup. The manual debug endpoint
    /// previews against the same one, so an operator's dry run shows exactly
    /// what the automatic sweep will do.
    const creatures::util::AudioCache::PrunePolicy &policy() const { return policy_; }

  protected:
    void run() override;

  private:
    /// Sleep in slices, returning false as soon as a stop was requested.
    bool sleepUnlessStopping(std::chrono::seconds total);

    /// One pass, with its own root span so each automatic run is visible.
    void sweep();

    std::shared_ptr<creatures::util::AudioCache> cache_;
    creatures::util::AudioCache::PrunePolicy policy_;
    std::chrono::seconds startupDelay_;
    std::chrono::seconds interval_;
};

} // namespace creatures::audio
