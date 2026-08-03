#include "LocalAudioPlaybackCoordinator.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <typeinfo>
#include <utility>

#include <spdlog/spdlog.h>

#include "util/ObservabilityManager.h"
#include "util/threadName.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

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

std::shared_ptr<OperationSpan> startPlaybackSpan(const LocalAudioPlaybackCoordinator::Job &job, uint64_t generation,
                                                 double queueWaitMs) {
    if (!observability) {
        return nullptr;
    }

    auto span = observability->createOperationSpan("audio.local.playback");
    if (!span) {
        return nullptr;
    }

    span->setAttribute("audio.local.generation", static_cast<int64_t>(generation));
    span->setAttribute("audio.local.job_id", job.id);
    span->setAttribute("audio.local.source", job.source);
    span->setAttribute("audio.local.mode", job.mode);
    if (!job.backend.empty()) {
        span->setAttribute("audio.local.backend", job.backend);
    }
    if (!job.deviceName.empty()) {
        span->setAttribute("audio.device.name", job.deviceName);
    }
    if (job.outputSampleRate > 0) {
        span->setAttribute("audio.output.sample_rate_hz", static_cast<int64_t>(job.outputSampleRate));
    }
    if (job.outputChannels > 0) {
        span->setAttribute("audio.output.channels", static_cast<int64_t>(job.outputChannels));
    }
    span->setAttribute("audio.file.name", job.fileName);
    std::string fileFormat = std::filesystem::path(job.fileName).extension().string();
    if (!fileFormat.empty() && fileFormat.front() == '.') {
        fileFormat.erase(fileFormat.begin());
    }
    std::transform(fileFormat.begin(), fileFormat.end(), fileFormat.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    if (!fileFormat.empty()) {
        span->setAttribute("audio.file.format", fileFormat);
    }
    span->setAttribute("audio.local.queue_capacity", static_cast<int64_t>(1));
    span->setAttribute("audio.local.queue_wait_ms", queueWaitMs);
    if (!job.sessionId.empty()) {
        span->setAttribute("session.id", job.sessionId);
    }
    if (!job.animationId.empty()) {
        span->setAttribute("animation.id", job.animationId);
    }
    if (job.universe) {
        span->setAttribute("session.universe", static_cast<int64_t>(*job.universe));
    }
    if (!job.triggerTraceId.empty()) {
        span->setAttribute("trigger.trace_id", job.triggerTraceId);
    }
    if (!job.triggerSpanId.empty()) {
        span->setAttribute("trigger.span_id", job.triggerSpanId);
    }
    return span;
}

void recordPlaybackException(const std::shared_ptr<OperationSpan> &span, const std::exception_ptr &exception) {
    if (!span || !exception) {
        return;
    }
    try {
        std::rethrow_exception(exception);
    } catch (const std::exception &caught) {
        span->recordException(caught);
    } catch (...) {
        const std::runtime_error unknownException("Unknown local audio playback exception");
        span->recordException(unknownException);
    }
}

void finishPlaybackSpan(const std::shared_ptr<OperationSpan> &span,
                        const LocalAudioPlaybackCoordinator::PlaybackResult &result, double playbackDurationMs) {
    if (!span) {
        return;
    }

    span->setAttribute("audio.local.outcome", LocalAudioPlaybackCoordinator::outcomeName(result.outcome));
    span->setAttribute("audio.local.playback_duration_ms", playbackDurationMs);
    span->setAttribute("audio.output.underruns", static_cast<int64_t>(result.deviceUnderruns));
    if (result.sourceFrames > 0) {
        span->setAttribute("audio.source.frames", static_cast<int64_t>(result.sourceFrames));
    }
    if (result.decodedFrames > 0) {
        span->setAttribute("audio.decoded.frames", static_cast<int64_t>(result.decodedFrames));
    }
    if (!result.stopReason.empty()) {
        span->setAttribute("audio.local.stop_reason", result.stopReason);
    }

    if (result.outcome == LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed ||
        result.outcome == LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut) {
        const std::string errorType =
            result.errorType.empty()
                ? (result.outcome == LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut ? "AudioPlaybackTimeout"
                                                                                              : "AudioPlaybackError")
                : result.errorType;
        const std::string errorCode = result.errorCode.empty()
                                          ? (result.outcome == LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut
                                                 ? "local_audio.playback_timeout"
                                                 : "local_audio.playback_failed")
                                          : result.errorCode;
        const std::string errorMessage =
            result.errorMessage.empty() ? "Local audio playback failed without an error message" : result.errorMessage;
        span->setAttribute("error.type", errorType);
        span->setAttribute("error.code", errorCode);
        span->setAttribute("error.message", errorMessage);
        span->setAttribute("audio.local.failure_stage", "playback");
        recordPlaybackException(span, result.exception);
        span->setError(errorMessage);
    } else {
        span->setSuccess();
    }
    span->end();
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
    : statsObserver_(std::move(statsObserver)) {
    // Launch only after every field the worker reads has completed
    // initialization. Member initialization order follows declaration order,
    // not the constructor initializer list.
    worker_ = std::thread(&LocalAudioPlaybackCoordinator::workerLoop, this);
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
                              .errorType =
                                  reason == StopReason::Replaced ? "AudioPlaybackReplaced" : "AudioPlaybackStopped",
                              .errorCode = reason == StopReason::Replaced ? "local_audio.replaced"
                                                                          : "local_audio.stopped_before_start",
                              .stopReason = reason == StopReason::Replaced ? "replaced" : "stopped"});
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
        finish(*discardedPending, PlaybackResult{.outcome = PlaybackOutcome::Shutdown,
                                                 .errorType = "AudioPlaybackShutdown",
                                                 .errorCode = "local_audio.shutdown_before_start",
                                                 .stopReason = "shutdown"});
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
            record.state->startedAt = std::chrono::steady_clock::now();
            record.state->status.store(Status::Playing, std::memory_order_release);
        }
        publishStats();

        const auto queueWait =
            std::chrono::duration<double, std::milli>(record.state->startedAt - record.state->queuedAt).count();
        std::shared_ptr<OperationSpan> playbackSpan;
        try {
            playbackSpan = startPlaybackSpan(record.job, record.state->generation, queueWait);
        } catch (const std::exception &exception) {
            spdlog::error("Local audio lifecycle span creation failed: {}", sanitizeLogField(exception.what()));
        } catch (...) {
            spdlog::error("Local audio lifecycle span creation failed with an unknown exception");
        }

        PlaybackResult result;
        const auto stopReasonBeforeStart = record.state->getStopReason();
        if (stopReasonBeforeStart != StopReason::None) {
            result.outcome = outcomeForStopReason(stopReasonBeforeStart);
            result.errorType = "AudioPlaybackCancelled";
            result.errorCode = "local_audio.cancelled_before_start";
            result.stopReason = stopReasonBeforeStart == StopReason::Replaced
                                    ? "replaced"
                                    : (stopReasonBeforeStart == StopReason::Shutdown ? "shutdown" : "stopped");
        } else {
            try {
                result = record.job.play(record.state->stopRequested);
            } catch (const std::exception &exception) {
                result = {.outcome = PlaybackOutcome::Failed,
                          .errorType = typeid(exception).name(),
                          .errorCode = "local_audio.playback_exception",
                          .errorMessage = exception.what(),
                          .exception = std::current_exception()};
            } catch (...) {
                result = {.outcome = PlaybackOutcome::Failed,
                          .errorType = "UnknownPlaybackException",
                          .errorCode = "local_audio.unknown_playback_exception",
                          .errorMessage = "Local audio playback threw an unknown exception",
                          .exception = std::current_exception()};
            }
        }

        {
            std::lock_guard lock(mutex_);
            const auto stopReason = record.state->getStopReason();
            if (stopReason != StopReason::None && result.outcome != PlaybackOutcome::Failed &&
                result.outcome != PlaybackOutcome::TimedOut) {
                result.outcome = outcomeForStopReason(stopReason);
                if (result.errorCode.empty()) {
                    result.errorCode = "local_audio." + std::string(outcomeName(result.outcome));
                }
            } else if (stopReason != StopReason::None) {
                result.stopReason = stopReason == StopReason::Replaced
                                        ? "replaced"
                                        : (stopReason == StopReason::Shutdown ? "shutdown" : "stopped");
            }
            if (activeState_ == record.state) {
                activeState_.reset();
                active_.store(0, std::memory_order_relaxed);
            }
        }

        finish(record, result);
        const auto playbackDuration =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - record.state->startedAt)
                .count();
        try {
            finishPlaybackSpan(playbackSpan, result, playbackDuration);
        } catch (const std::exception &exception) {
            spdlog::error("Local audio lifecycle span completion failed: {}", sanitizeLogField(exception.what()));
        } catch (...) {
            spdlog::error("Local audio lifecycle span completion failed with an unknown exception");
        }
        publishStats();
    }
}

void LocalAudioPlaybackCoordinator::finish(JobRecord &record, const PlaybackResult &result) {
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
}

void LocalAudioPlaybackCoordinator::publishStats() const {
    if (!statsObserver_) {
        return;
    }
    std::lock_guard observerLock(statsObserverMutex_);
    try {
        statsObserver_(getStats());
    } catch (const std::exception &exception) {
        spdlog::error("Local audio stats observer failed: {}", sanitizeLogField(exception.what()));
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
