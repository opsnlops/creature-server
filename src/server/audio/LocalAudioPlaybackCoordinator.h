#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace creatures::audio {

/**
 * Single-worker coordinator for the process-global SDL audio device.
 *
 * SDL and SDL_mixer device lifecycle is process-global in this application.
 * Both animation transports and the ad-hoc sound endpoint submit here so only
 * one owner can open, play, and close the configured device at a time.
 *
 * Submission is last-request-wins and never waits for playback: the active
 * request is asked to stop, the single pending slot is replaced, and the sole
 * worker performs all blocking device work.
 */
class LocalAudioPlaybackCoordinator {
  public:
    enum class PlaybackOutcome {
        Completed,
        Replaced,
        Stopped,
        Failed,
        TimedOut,
        Shutdown,
    };

    struct PlaybackResult {
        PlaybackOutcome outcome{PlaybackOutcome::Completed};
        std::string errorType;
        std::string errorCode;
        std::string errorMessage;
        std::string stopReason;
        std::exception_ptr exception;
    };

    struct Stats {
        std::size_t workerCount{1};
        std::size_t queueCapacity{1};
        std::size_t active{0};
        std::size_t queued{0};
        uint64_t accepted{0};
        uint64_t completed{0};
        uint64_t replaced{0};
        uint64_t rejected{0};
        uint64_t stopped{0};
        uint64_t failed{0};
        uint64_t timedOut{0};
    };

    struct Job {
        std::string id;
        std::string source;
        std::string mode;
        std::string fileName;
        std::string sessionId;
        std::string animationId;
        std::optional<uint32_t> universe;
        std::string triggerTraceId;
        std::string triggerSpanId;
        std::function<PlaybackResult(const std::atomic<bool> &stopRequested)> play;
    };

    enum class SubmitResult {
        Accepted,
        ShuttingDown,
        InvalidJob,
    };

  private:
    enum class Status {
        Queued,
        Playing,
        Completed,
        Replaced,
        Stopped,
        Failed,
        TimedOut,
        Shutdown,
    };

    enum class StopReason {
        None,
        Replaced,
        Stopped,
        Shutdown,
    };

    struct State {
        explicit State(uint64_t generationValue) : generation(generationValue) {}

        bool requestStop(StopReason reason);
        [[nodiscard]] StopReason getStopReason() const;

        const uint64_t generation;
        std::atomic<Status> status{Status::Queued};
        std::atomic<StopReason> stopReason{StopReason::None};
        std::atomic<bool> stopRequested{false};
        std::atomic<bool> completionNotified{false};
        const std::chrono::steady_clock::time_point queuedAt{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point startedAt{};
    };

  public:
    class Handle {
      public:
        Handle() = default;

        void stop() const;
        [[nodiscard]] bool isFinished() const;
        [[nodiscard]] uint64_t generation() const;

      private:
        friend class LocalAudioPlaybackCoordinator;
        explicit Handle(std::shared_ptr<State> state) : state_(std::move(state)) {}

        std::shared_ptr<State> state_;
    };

    struct Submission {
        SubmitResult result{SubmitResult::ShuttingDown};
        std::shared_ptr<Handle> handle;
    };

    using StatsObserver = std::function<void(const Stats &)>;

    explicit LocalAudioPlaybackCoordinator(StatsObserver statsObserver = {});
    ~LocalAudioPlaybackCoordinator();

    LocalAudioPlaybackCoordinator(const LocalAudioPlaybackCoordinator &) = delete;
    LocalAudioPlaybackCoordinator &operator=(const LocalAudioPlaybackCoordinator &) = delete;
    LocalAudioPlaybackCoordinator(LocalAudioPlaybackCoordinator &&) = delete;
    LocalAudioPlaybackCoordinator &operator=(LocalAudioPlaybackCoordinator &&) = delete;

    [[nodiscard]] Submission submit(Job job);

    /**
     * Stop accepting work, request cooperative cancellation, and join the sole
     * device worker. Safe to call more than once.
     */
    void shutdown();

    [[nodiscard]] bool isStopping() const { return stopRequested_.load(std::memory_order_acquire); }
    [[nodiscard]] Stats getStats() const;

    static const char *outcomeName(PlaybackOutcome outcome);

  private:
    struct JobRecord {
        Job job;
        std::shared_ptr<State> state;
    };

    void workerLoop();
    void finish(JobRecord &record, const PlaybackResult &result);
    void publishStats() const;
    static PlaybackOutcome outcomeForStopReason(StopReason reason);
    static Status statusForOutcome(PlaybackOutcome outcome);

    const StatsObserver statsObserver_;
    mutable std::mutex statsObserverMutex_;
    mutable std::mutex mutex_;
    std::mutex shutdownMutex_;
    std::condition_variable condition_;
    std::optional<JobRecord> pending_;
    std::shared_ptr<State> activeState_;
    bool shuttingDown_{false};
    uint64_t nextGeneration_{1};

    std::atomic<bool> stopRequested_{false};
    std::atomic<std::size_t> active_{0};
    std::atomic<std::size_t> queued_{0};
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> replaced_{0};
    std::atomic<uint64_t> rejected_{0};
    std::atomic<uint64_t> stopped_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<uint64_t> timedOut_{0};
    std::thread worker_;
};

} // namespace creatures::audio
