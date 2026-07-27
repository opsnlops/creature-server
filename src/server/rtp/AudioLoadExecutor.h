#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <latch>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace creatures::rtp {

/**
 * Fixed-size executor for cooperative animation RTP audio loads.
 *
 * Submission never waits for queue space: callers get an explicit rejection
 * when the bounded queue is full or shutdown has started. A cancellation
 * predicate is checked after dequeue and before the expensive WAV read/encode,
 * so sessions replaced while queued are abandoned cheaply.
 */
class AudioLoadExecutor {
  public:
    struct Stats {
        std::size_t workerCount{0};
        std::size_t queueCapacity{0};
        std::size_t active{0};
        std::size_t queued{0};
        uint64_t accepted{0};
        uint64_t completed{0};
        uint64_t rejected{0};
        uint64_t cancelled{0};
        uint64_t failed{0};
    };

    struct Job {
        std::string id;
        // Called while the executor's queue mutex is held when pruning stale
        // work, so this must stay cheap and should not throw.
        std::function<bool()> isCancelled;
        std::function<void()> run;
        std::function<void(std::exception_ptr)> onFailure;
    };

    enum class SubmitResult {
        Accepted,
        QueueFull,
        ShuttingDown,
    };

    using StatsObserver = std::function<void(const Stats &)>;

    AudioLoadExecutor(std::size_t workerCount, std::size_t queueCapacity, StatsObserver statsObserver = {});
    ~AudioLoadExecutor();

    AudioLoadExecutor(const AudioLoadExecutor &) = delete;
    AudioLoadExecutor &operator=(const AudioLoadExecutor &) = delete;
    AudioLoadExecutor(AudioLoadExecutor &&) = delete;
    AudioLoadExecutor &operator=(AudioLoadExecutor &&) = delete;

    /**
     * Try to enqueue a job without waiting for queue space.
     */
    [[nodiscard]] SubmitResult submit(Job job);

    /**
     * Stop accepting work, discard queued jobs, and join active workers.
     * Safe to call more than once.
     */
    void shutdown();

    [[nodiscard]] Stats getStats() const;

  private:
    std::size_t discardCancelledJobsLocked();
    void workerLoop(std::size_t workerIndex);
    void publishStats() const;
    void recordFailure(const Job &job, std::exception_ptr failure);

    const std::size_t workerCount_;
    const std::size_t queueCapacity_;
    const StatsObserver statsObserver_;
    mutable std::mutex statsObserverMutex_;

    mutable std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::deque<Job> queue_;
    std::latch workersStart_{1};
    std::vector<std::thread> workers_;
    bool shuttingDown_{false};

    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> queued_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> cancelled_{0};
    std::atomic<uint64_t> failed_{0};
};

} // namespace creatures::rtp
