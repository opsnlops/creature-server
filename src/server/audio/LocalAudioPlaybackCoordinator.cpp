#include "LocalAudioPlaybackCoordinator.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

#include "util/threadName.h"

namespace creatures::audio {

namespace {

std::string sanitizeLogField(std::string_view value, std::size_t maxLength = 512) {
    const auto length = std::min(value.size(), maxLength);
    std::string sanitized;
    sanitized.reserve(length);
    for (std::size_t index = 0; index < length; ++index) {
        const unsigned char character = static_cast<unsigned char>(value[index]);
        sanitized.push_back(character >= 0x20 && character <= 0x7e ? static_cast<char>(character) : '_');
    }
    if (value.size() > maxLength) {
        sanitized.append("...");
    }
    return sanitized;
}

} // namespace

bool LocalAudioPlaybackCoordinator::State::requestStop(StopReason reason) {
    StopReason expected = StopReason::None;
    if (!stopReason.compare_exchange_strong(expected, reason, std::memory_order_acq_rel)) {
        return false;
    }
    stopRequested.store(true, std::memory_order_release);
    return true;
}

LocalAudioPlaybackCoordinator::StopReason LocalAudioPlaybackCoordinator::State::getStopReason() const {
    return stopReason.load(std::memory_order_acquire);
}

void LocalAudioPlaybackCoordinator::Handle::stop() const {
    if (state_) {
        state_->requestStop(StopReason::Stopped);
    }
}

bool LocalAudioPlaybackCoordinator::Handle::isFinished() const {
    if (!state_) {
        return true;
    }
    switch (state_->status.load(std::memory_order_acquire)) {
    case Status::Queued:
    case Status::Playing:
        return false;
    case Status::Completed:
    case Status::Replaced:
    case Status::Stopped:
    case Status::Failed:
    case Status::TimedOut:
    case Status::Shutdown:
        return true;
    }
    return true;
}

uint64_t LocalAudioPlaybackCoordinator::Handle::generation() const { return state_ ? state_->generation : 0; }

LocalAudioPlaybackCoordinator::LocalAudioPlaybackCoordinator(StatsObserver statsObserver)
    : statsObserver_(std::move(statsObserver)), worker_(&LocalAudioPlaybackCoordinator::workerLoop, this) {
    publishStats();
}

LocalAudioPlaybackCoordinator::~LocalAudioPlaybackCoordinator() { shutdown(); }

LocalAudioPlaybackCoordinator::Submission LocalAudioPlaybackCoordinator::submit(Job job) {
    if (!job.play) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        publishStats();
        return {.result = SubmitResult::InvalidJob};
    }

    std::optional<JobRecord> displacedPending;
    std::shared_ptr<Handle> handle;
    bool shuttingDown = false;
    {
        std::lock_guard lock(mutex_);
        if (shuttingDown_) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            shuttingDown = true;
        } else {
            auto state = std::make_shared<State>(nextGeneration_++);
            handle = std::shared_ptr<Handle>(new Handle(state));

            if (activeState_ && activeState_->requestStop(StopReason::Replaced)) {
                replaced_.fetch_add(1, std::memory_order_relaxed);
            }

            if (pending_) {
                if (pending_->state->requestStop(StopReason::Replaced)) {
                    replaced_.fetch_add(1, std::memory_order_relaxed);
                }
                displacedPending = std::move(pending_);
            }

            pending_ = JobRecord{.job = std::move(job), .state = std::move(state)};
            queued_.store(1, std::memory_order_relaxed);
            accepted_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if (shuttingDown) {
        publishStats();
        return {.result = SubmitResult::ShuttingDown};
    }
    if (displacedPending) {
        const auto reason = displacedPending->state->getStopReason();
        finish(*displacedPending,
               PlaybackResult{.outcome = outcomeForStopReason(reason),
                              .errorCode = reason == StopReason::Replaced ? "local_audio.replaced"
                                                                          : "local_audio.stopped_before_start"});
    }
    publishStats();
    condition_.notify_one();
    return {.result = SubmitResult::Accepted, .handle = std::move(handle)};
}

void LocalAudioPlaybackCoordinator::shutdown() {
    std::lock_guard shutdownLock(shutdownMutex_);
    if (!worker_.joinable()) {
        return;
    }

    std::optional<JobRecord> discardedPending;
    stopRequested_.store(true, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        shuttingDown_ = true;
        if (activeState_) {
            activeState_->requestStop(StopReason::Shutdown);
        }
        if (pending_) {
            pending_->state->requestStop(StopReason::Shutdown);
            discardedPending = std::move(pending_);
            pending_.reset();
            queued_.store(0, std::memory_order_relaxed);
        }
    }

    if (discardedPending) {
        finish(*discardedPending,
               PlaybackResult{.outcome = PlaybackOutcome::Shutdown, .errorCode = "local_audio.shutdown_before_start"});
    }
    publishStats();
    condition_.notify_all();
    worker_.join();
    publishStats();
}

LocalAudioPlaybackCoordinator::Stats LocalAudioPlaybackCoordinator::getStats() const {
    return Stats{
        .active = active_.load(std::memory_order_relaxed),
        .queued = queued_.load(std::memory_order_relaxed),
        .accepted = accepted_.load(std::memory_order_relaxed),
        .completed = completed_.load(std::memory_order_relaxed),
        .replaced = replaced_.load(std::memory_order_relaxed),
        .rejected = rejected_.load(std::memory_order_relaxed),
        .stopped = stopped_.load(std::memory_order_relaxed),
        .failed = failed_.load(std::memory_order_relaxed),
        .timedOut = timedOut_.load(std::memory_order_relaxed),
    };
}

const char *LocalAudioPlaybackCoordinator::outcomeName(PlaybackOutcome outcome) {
    switch (outcome) {
    case PlaybackOutcome::Completed:
        return "completed";
    case PlaybackOutcome::Replaced:
        return "replaced";
    case PlaybackOutcome::Stopped:
        return "stopped";
    case PlaybackOutcome::Failed:
        return "failed";
    case PlaybackOutcome::TimedOut:
        return "timed_out";
    case PlaybackOutcome::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

void LocalAudioPlaybackCoordinator::workerLoop() {
    setThreadName("creature-server::local-audio");

    while (true) {
        JobRecord record;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this]() { return shuttingDown_ || pending_.has_value(); });
            if (shuttingDown_ && !pending_) {
                return;
            }

            record = std::move(*pending_);
            pending_.reset();
            queued_.store(0, std::memory_order_relaxed);
            activeState_ = record.state;
            active_.store(1, std::memory_order_relaxed);
            record.state->status.store(Status::Playing, std::memory_order_release);
        }
        publishStats();

        PlaybackResult result;
        const auto stopReasonBeforeStart = record.state->getStopReason();
        if (stopReasonBeforeStart != StopReason::None) {
            result.outcome = outcomeForStopReason(stopReasonBeforeStart);
            result.errorCode = "local_audio.cancelled_before_start";
        } else {
            try {
                result = record.job.play(record.state->stopRequested);
            } catch (const std::exception &exception) {
                result = {.outcome = PlaybackOutcome::Failed,
                          .errorCode = "local_audio.playback_exception",
                          .errorMessage = exception.what()};
            } catch (...) {
                result = {.outcome = PlaybackOutcome::Failed,
                          .errorCode = "local_audio.unknown_playback_exception",
                          .errorMessage = "Local audio playback threw an unknown exception"};
            }
        }

        {
            std::lock_guard lock(mutex_);
            const auto stopReason = record.state->getStopReason();
            if (stopReason != StopReason::None) {
                result.outcome = outcomeForStopReason(stopReason);
                if (result.errorCode.empty()) {
                    result.errorCode = "local_audio." + std::string(outcomeName(result.outcome));
                }
            }
            if (activeState_ == record.state) {
                activeState_.reset();
                active_.store(0, std::memory_order_relaxed);
            }
        }

        finish(record, std::move(result));
        publishStats();
    }
}

void LocalAudioPlaybackCoordinator::finish(JobRecord &record, PlaybackResult result) {
    bool expected = false;
    if (!record.state->completionNotified.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return;
    }

    switch (result.outcome) {
    case PlaybackOutcome::Completed:
        completed_.fetch_add(1, std::memory_order_relaxed);
        break;
    case PlaybackOutcome::Replaced:
        break;
    case PlaybackOutcome::Stopped:
    case PlaybackOutcome::Shutdown:
        stopped_.fetch_add(1, std::memory_order_relaxed);
        break;
    case PlaybackOutcome::Failed:
        failed_.fetch_add(1, std::memory_order_relaxed);
        break;
    case PlaybackOutcome::TimedOut:
        timedOut_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    // Publish the terminal state after its counters so Handle::isFinished()
    // acts as an acquire/release completion barrier for callers and tests.
    record.state->status.store(statusForOutcome(result.outcome), std::memory_order_release);

    if (result.outcome == PlaybackOutcome::Failed || result.outcome == PlaybackOutcome::TimedOut) {
        spdlog::error(
            "local_audio_lifecycle outcome={} error.code={} generation={} source={} file_name={} job_id={} message={}",
            outcomeName(result.outcome), sanitizeLogField(result.errorCode), record.state->generation,
            sanitizeLogField(record.job.source), sanitizeLogField(record.job.fileName), sanitizeLogField(record.job.id),
            sanitizeLogField(result.errorMessage));
    }

    if (!record.job.onFinished) {
        return;
    }

    try {
        record.job.onFinished(Completion{
            .generation = record.state->generation,
            .id = record.job.id,
            .source = record.job.source,
            .fileName = record.job.fileName,
            .result = std::move(result),
        });
    } catch (const std::exception &exception) {
        spdlog::error("Local audio job '{}' completion handler threw: {}", sanitizeLogField(record.job.id),
                      sanitizeLogField(exception.what()));
    } catch (...) {
        spdlog::error("Local audio job '{}' completion handler threw an unknown exception",
                      sanitizeLogField(record.job.id));
    }
}

void LocalAudioPlaybackCoordinator::publishStats() const {
    if (!statsObserver_) {
        return;
    }
    std::lock_guard observerLock(statsObserverMutex_);
    try {
        statsObserver_(getStats());
    } catch (const std::exception &exception) {
        spdlog::error("Local audio stats observer failed: {}", exception.what());
    } catch (...) {
        spdlog::error("Local audio stats observer failed with an unknown exception");
    }
}

LocalAudioPlaybackCoordinator::PlaybackOutcome LocalAudioPlaybackCoordinator::outcomeForStopReason(StopReason reason) {
    switch (reason) {
    case StopReason::None:
        return PlaybackOutcome::Completed;
    case StopReason::Replaced:
        return PlaybackOutcome::Replaced;
    case StopReason::Stopped:
        return PlaybackOutcome::Stopped;
    case StopReason::Shutdown:
        return PlaybackOutcome::Shutdown;
    }
    return PlaybackOutcome::Failed;
}

LocalAudioPlaybackCoordinator::Status LocalAudioPlaybackCoordinator::statusForOutcome(PlaybackOutcome outcome) {
    switch (outcome) {
    case PlaybackOutcome::Completed:
        return Status::Completed;
    case PlaybackOutcome::Replaced:
        return Status::Replaced;
    case PlaybackOutcome::Stopped:
        return Status::Stopped;
    case PlaybackOutcome::Failed:
        return Status::Failed;
    case PlaybackOutcome::TimedOut:
        return Status::TimedOut;
    case PlaybackOutcome::Shutdown:
        return Status::Shutdown;
    }
    return Status::Failed;
}

} // namespace creatures::audio
