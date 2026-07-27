#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace creatures::rtp {

/**
 * Identifies one exclusive owner of the process-wide RTP multicast output.
 *
 * Generations are monotonically increasing so work from a released owner can
 * never become current again, even after the output sits idle.
 */
struct RtpOutputLease {
    uint64_t generation{0};
    std::string ownerId;

    [[nodiscard]] bool valid() const { return generation != 0 && !ownerId.empty(); }
};

/**
 * Serializes ownership changes with complete 17-channel output operations.
 *
 * The output worker holds Guard while it mutates SSRC/timestamp state and
 * sends one complete channel set. acquire() waits for that small critical
 * section, then invalidates every command from the previous generation.
 */
class RtpOutputCoordinator {
  public:
    class Guard {
      public:
        Guard(Guard &&) noexcept = default;
        Guard &operator=(Guard &&) noexcept = default;

        Guard(const Guard &) = delete;
        Guard &operator=(const Guard &) = delete;

        [[nodiscard]] explicit operator bool() const { return current_; }

      private:
        friend class RtpOutputCoordinator;

        Guard(std::unique_lock<std::mutex> lock, bool current) : lock_(std::move(lock)), current_(current) {}

        std::unique_lock<std::mutex> lock_;
        bool current_{false};
    };

    [[nodiscard]] RtpOutputLease acquire(std::string ownerId) {
        std::lock_guard lock(mutex_);
        ++nextGeneration_;
        if (nextGeneration_ == 0) {
            ++nextGeneration_;
        }
        activeGeneration_.store(nextGeneration_);
        activeOwnerId_ = std::move(ownerId);
        return RtpOutputLease{nextGeneration_, activeOwnerId_};
    }

    [[nodiscard]] bool isCurrent(const RtpOutputLease &lease) const {
        return lease.valid() && lease.generation == activeGeneration_.load();
    }

    [[nodiscard]] Guard lockIfCurrent(const RtpOutputLease &lease) {
        std::unique_lock lock(mutex_);
        const bool current = lease.valid() && lease.generation == activeGeneration_.load();
        return Guard(std::move(lock), current);
    }

    void release(const RtpOutputLease &lease) {
        if (!lease.valid()) {
            return;
        }

        uint64_t expected = lease.generation;
        if (activeGeneration_.compare_exchange_strong(expected, 0)) {
            // Clearing the diagnostic owner is best effort: release() is called
            // from the 1ms event loop and must never wait for an in-flight send.
            std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock.owns_lock() && activeGeneration_.load() == 0) {
                activeOwnerId_.clear();
            }
        }
    }

    [[nodiscard]] uint64_t activeGeneration() const { return activeGeneration_.load(); }

    [[nodiscard]] std::string activeOwnerId() const {
        if (activeGeneration_.load() == 0) {
            return {};
        }
        std::lock_guard lock(mutex_);
        if (activeGeneration_.load() == 0) {
            return {};
        }
        return activeOwnerId_;
    }

  private:
    mutable std::mutex mutex_;
    uint64_t nextGeneration_{0};
    std::atomic<uint64_t> activeGeneration_{0};
    std::string activeOwnerId_;
};

} // namespace creatures::rtp
