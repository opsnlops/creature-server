#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "server/rtp/AsyncAudioTraceContext.h"

namespace creatures::rtp {

/**
 * Tuning for the RTP send-failure circuit breaker (issue #97).
 *
 * At the 10 ms frame cadence the output worker attempts ~100 frame sets per
 * second, so the consecutive threshold trips roughly half a second into a
 * hard socket failure, while the windowed threshold catches persistent
 * flapping that keeps resetting the consecutive count.
 */
struct RtpBreakerConfig {
    uint32_t consecutiveFailuresToTrip{50};
    uint32_t windowedFailuresToTrip{300};
    uint32_t windowSeconds{10};
    // Detailed log/span emission: always on failure #1 of a run, then every
    // Nth — the same gate AlsaAudioOutput and the native keepalive use.
    uint32_t detailEmitInterval{100};
};

/**
 * Consecutive + windowed send-failure tracking for one RTP output generation.
 *
 * Owned and driven solely by the RTP output worker thread — no locking. Time
 * is injected so tests are deterministic. State is scoped to a generation:
 * feeding a different generation implicitly resets, so a stale generation can
 * never contribute to a newer one's trip decision.
 */
class RtpSendFailureTracker {
  public:
    struct Action {
        bool emitDetail{false};
        bool trip{false};
        bool recovered{false};
        uint64_t suppressedSinceLastEmit{0};
        uint64_t consecutiveFailures{0};
        uint64_t windowedFailures{0};
    };

    explicit RtpSendFailureTracker(RtpBreakerConfig config = {}) : config_(config) {
        if (config_.windowSeconds == 0) {
            config_.windowSeconds = 1;
        }
        buckets_.assign(config_.windowSeconds, 0);
    }

    void beginGeneration(uint64_t generation, std::chrono::steady_clock::time_point now) {
        generation_ = generation;
        tripped_ = false;
        consecutiveFailures_ = 0;
        windowedFailures_ = 0;
        suppressedSinceLastEmit_ = 0;
        buckets_.assign(config_.windowSeconds, 0);
        bucketIndex_ = 0;
        bucketTime_ = now;
    }

    [[nodiscard]] Action recordFailure(uint64_t generation, std::chrono::steady_clock::time_point now) {
        if (generation != generation_) {
            beginGeneration(generation, now);
        }
        Action action;
        if (tripped_) {
            // The worker already released this generation; anything still
            // draining is stale. Stay silent so a trip emits exactly once.
            return action;
        }
        advanceWindow(now);
        ++consecutiveFailures_;
        ++buckets_[bucketIndex_];
        ++windowedFailures_;

        action.consecutiveFailures = consecutiveFailures_;
        action.windowedFailures = windowedFailures_;
        action.emitDetail = consecutiveFailures_ == 1 ||
                            (config_.detailEmitInterval > 0 && consecutiveFailures_ % config_.detailEmitInterval == 0);
        if (consecutiveFailures_ >= config_.consecutiveFailuresToTrip ||
            windowedFailures_ >= config_.windowedFailuresToTrip) {
            tripped_ = true;
            action.trip = true;
            action.emitDetail = true;
        }
        if (action.emitDetail) {
            action.suppressedSinceLastEmit = suppressedSinceLastEmit_;
            suppressedSinceLastEmit_ = 0;
        } else {
            ++suppressedSinceLastEmit_;
        }
        return action;
    }

    [[nodiscard]] Action recordSuccess(uint64_t generation, std::chrono::steady_clock::time_point now) {
        if (generation != generation_) {
            beginGeneration(generation, now);
            return {};
        }
        if (tripped_) {
            return {};
        }
        advanceWindow(now);
        Action action;
        if (consecutiveFailures_ > 0) {
            action.recovered = true;
            action.consecutiveFailures = consecutiveFailures_;
            action.windowedFailures = windowedFailures_;
            action.suppressedSinceLastEmit = suppressedSinceLastEmit_;
            consecutiveFailures_ = 0;
            suppressedSinceLastEmit_ = 0;
        }
        return action;
    }

    [[nodiscard]] bool isTripped() const { return tripped_; }
    [[nodiscard]] uint64_t generation() const { return generation_; }

  private:
    void advanceWindow(std::chrono::steady_clock::time_point now) {
        using std::chrono::seconds;
        auto elapsed = std::chrono::duration_cast<seconds>(now - bucketTime_).count();
        if (elapsed <= 0) {
            return;
        }
        const auto steps = std::min<int64_t>(elapsed, static_cast<int64_t>(config_.windowSeconds));
        for (int64_t i = 0; i < steps; ++i) {
            bucketIndex_ = (bucketIndex_ + 1) % config_.windowSeconds;
            windowedFailures_ -= buckets_[bucketIndex_];
            buckets_[bucketIndex_] = 0;
        }
        bucketTime_ += seconds(elapsed);
    }

    RtpBreakerConfig config_;
    uint64_t generation_{0};
    bool tripped_{false};
    uint64_t consecutiveFailures_{0};
    uint64_t windowedFailures_{0};
    uint64_t suppressedSinceLastEmit_{0};
    std::vector<uint64_t> buckets_;
    size_t bucketIndex_{0};
    std::chrono::steady_clock::time_point bucketTime_{};
};

/**
 * The terminal failure the circuit breaker publishes when it trips.
 *
 * Carries enough context to answer "which playback died, and why" from the
 * event loop after the lease has already been released.
 */
struct TerminalFailure {
    uint64_t generation{0};
    int errorCode{0}; // rtp_error_t value, or 0 when the send threw instead
    std::string errorMessage;
    uint8_t firstFailedChannel{0};
    uint32_t rtpTimestamp{0};
    std::string commandType;
    uint64_t frameIndex{0};
    uint64_t consecutiveFailures{0};
    std::string ownerId;
    AsyncAudioTraceContext traceContext;
};

/**
 * Worker-to-event-loop channel for terminal RTP output failures.
 *
 * The worker releases a tripped generation's lease, so "no longer current"
 * alone is indistinguishable from a benign supersede. This registry keeps the
 * last few tripped generations queryable after release. The event-loop fast
 * path (isTripped) is a handful of relaxed atomic loads — no lock unless a
 * generation actually tripped.
 */
class RtpTerminalFailureRegistry {
  public:
    static constexpr size_t RING_SIZE = 4;

    void publish(TerminalFailure failure) {
        const uint64_t generation = failure.generation;
        if (generation == 0) {
            return;
        }
        size_t slot = 0;
        {
            std::lock_guard lock(mutex_);
            slot = nextSlot_ % RING_SIZE;
            entries_[slot] = std::move(failure);
            ++nextSlot_;
        }
        // Generation becomes visible only after the entry is fully written, so
        // a reader that sees isTripped() can always find() the details.
        generations_[slot].store(generation, std::memory_order_release);
    }

    [[nodiscard]] bool isTripped(uint64_t generation) const noexcept {
        if (generation == 0) {
            return false;
        }
        for (const auto &slot : generations_) {
            if (slot.load(std::memory_order_acquire) == generation) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::optional<TerminalFailure> find(uint64_t generation) const {
        if (!isTripped(generation)) {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        for (const auto &entry : entries_) {
            if (entry.generation == generation) {
                return entry;
            }
        }
        return std::nullopt;
    }

  private:
    mutable std::mutex mutex_;
    std::array<TerminalFailure, RING_SIZE> entries_{};
    std::array<std::atomic<uint64_t>, RING_SIZE> generations_{};
    size_t nextSlot_{0};
};

} // namespace creatures::rtp
