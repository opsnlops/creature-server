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
    enum class CancellationReason {
        SessionCancelled,
        Shutdown,
    };

    struct Stats {
        std::size_t workerCount{0};
        std::size_t queueCapacity{0};
        std::size_t active{0};
        std::size_t queued{0};
        std::size_t reserved{0};
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
        std::function<void(CancellationReason)> onCancelled;
    };

    enum class SubmitResult {
        Accepted,
        QueueFull,
        ShuttingDown,
    };

    /**
     * Move-only admission token. Reserving before expensive PlaybackSession
     * construction guarantees that a later commit cannot fail for saturation
     * after the caller has displaced an existing session.
     *
     * A reservation must not outlive its executor.
     */
    class Reservation {
      public:
        Reservation() = default;
        ~Reservation();

        Reservation(const Reservation &) = delete;
        Reservation &operator=(const Reservation &) = delete;
        Reservation(Reservation &&other) noexcept;
        Reservation &operator=(Reservation &&other) noexcept;

        [[nodiscard]] bool valid() const { return owner_ != nullptr; }

      private:
        friend class AudioLoadExecutor;
        explicit Reservation(AudioLoadExecutor *owner) : owner_(owner) {}
        void consume() { owner_ = nullptr; }

        AudioLoadExecutor *owner_{nullptr};
    };

    struct ReserveResult {
        SubmitResult result{SubmitResult::ShuttingDown};
        Reservation reservation;
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
     * Reserve one bounded in-flight slot before doing expensive preparation or
     * mutating playback ownership. The token releases itself if not committed.
     */
    [[nodiscard]] ReserveResult tryReserve();

    /**
     * Commit a previously reserved job. Saturation cannot reject a valid token;
     * shutdown waits for outstanding reservations to commit or release.
     */
    [[nodiscard]] SubmitResult submit(Reservation reservation, Job job);

    /**
     * Stop accepting work, discard queued jobs, and join active workers.
     * Safe to call more than once.
     */
    void shutdown();

    [[nodiscard]] bool isStopping() const { return stopRequested_.load(std::memory_order_acquire); }

    [[nodiscard]] Stats getStats() const;

  private:
    struct CancellationNotification {
        std::string id;
        CancellationReason reason{CancellationReason::SessionCancelled};
        std::function<void(CancellationReason)> callback;
    };

    std::vector<Job> takeCancelledJobsLocked();
    void enqueueCancellationNotificationsLocked(std::vector<Job> &jobs, CancellationReason reason);
    void notifyCancelledJob(const Job &job, CancellationReason reason);
    void processCancellationNotification(CancellationNotification notification);
    void releaseReservation();
    void workerLoop(std::size_t workerIndex);
    void publishStats() const;
    void recordFailure(const Job &job, std::exception_ptr failure);

    const std::size_t workerCount_;
    const std::size_t queueCapacity_;
    const StatsObserver statsObserver_;
    mutable std::mutex statsObserverMutex_;

    mutable std::mutex queueMutex_;
    std::condition_variable queueCondition_;
    std::condition_variable reservationsCondition_;
    std::deque<Job> queue_;
    std::deque<CancellationNotification> cancellationNotifications_;
    std::latch workersStart_{1};
    std::vector<std::thread> workers_;
    bool accepting_{true};
    bool shuttingDown_{false};
    std::size_t reservations_{0};
    std::size_t inFlight_{0};

    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> queued_{0};
    std::atomic<std::size_t> reserved_{0};
    std::atomic<bool> stopRequested_{false};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> cancelled_{0};
    std::atomic<uint64_t> failed_{0};
};

} // namespace creatures::rtp
