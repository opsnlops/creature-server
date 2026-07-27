#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace creatures::rtp {

/**
 * Small non-blocking producer queue for event-loop-to-worker handoff.
 *
 * Producers never wait for capacity. The worker is the sole blocking consumer.
 * stop() discards pending work so server shutdown cannot replay stale audio.
 */
template <typename Command> class BoundedCommandQueue {
  public:
    explicit BoundedCommandQueue(size_t capacity) : capacity_(capacity) {}

    BoundedCommandQueue(const BoundedCommandQueue &) = delete;
    BoundedCommandQueue &operator=(const BoundedCommandQueue &) = delete;

    [[nodiscard]] bool tryPush(Command command) {
        {
            std::lock_guard lock(mutex_);
            if (stopped_ || queue_.size() >= capacity_) {
                return false;
            }
            queue_.push_back(std::move(command));
        }
        condition_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<Command> waitPop() {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
        if (stopped_) {
            return std::nullopt;
        }

        Command command = std::move(queue_.front());
        queue_.pop_front();
        return command;
    }

    template <typename Predicate> size_t eraseIf(Predicate predicate) {
        std::lock_guard lock(mutex_);
        const size_t originalSize = queue_.size();
        std::erase_if(queue_, predicate);
        return originalSize - queue_.size();
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            queue_.clear();
        }
        condition_.notify_all();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] size_t capacity() const { return capacity_; }

  private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<Command> queue_;
    bool stopped_{false};
};

} // namespace creatures::rtp
