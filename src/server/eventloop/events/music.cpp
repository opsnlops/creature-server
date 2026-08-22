//
// music.cpp
//
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>

#include <spdlog/spdlog.h>

#include "server/audio/LocalAudioPlaybackCoordinator.h"
#include "server/audio/LocalNativeAudioTransport.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "util/ObservabilityManager.h"

namespace creatures {

// ───── singletons from main.cpp ──────────────────────────────────────────
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<audio::LocalAudioPlaybackCoordinator> localAudioPlaybackCoordinator;

// Use constants from config.h - 17 channels for 16 creatures + BGM

namespace {

/**
 * Reports the outcome of a standalone RTP playback after its final frame was
 * enqueued (issue #97).
 *
 * The output worker releases the lease after actually sending the final frame
 * set, so "lease no longer current" is the delivery acknowledgment — unless
 * the send-failure circuit breaker terminated the generation, which the
 * terminal-failure registry distinguishes. Success (the soundsPlayed counter
 * and span outcome) is only recorded here, never at enqueue time.
 */
class StandaloneRtpCompletionCheckEvent : public EventBase<StandaloneRtpCompletionCheckEvent> {
  public:
    // The output queue holds at most 64 commands draining at 100/s (~640 ms);
    // give the worker 2 s before declaring it stuck.
    static constexpr uint32_t MAX_ATTEMPTS = 200;

    StandaloneRtpCompletionCheckEvent(framenum_t frameNumber, rtp::RtpOutputLease outputLease,
                                      std::shared_ptr<OperationSpan> span, size_t skippedFramesTotal,
                                      uint32_t attempts = 0)
        : EventBase(frameNumber), outputLease_(std::move(outputLease)), span_(std::move(span)),
          skippedFramesTotal_(skippedFramesTotal), attempts_(attempts) {}

    Result<framenum_t> executeImpl() {
        if (!rtpServer) {
            return fail("RTP server unavailable during standalone completion check");
        }

        if (rtpServer->isCurrentOutput(outputLease_)) {
            // A trip publishes its record before releasing, so a tripped-but-
            // still-current read means the release simply isn't visible yet.
            if (rtpServer->isGenerationTripped(outputLease_.generation)) {
                return failTerminal();
            }
            // The worker hasn't processed the final frame set yet.
            if (attempts_ + 1 >= MAX_ATTEMPTS) {
                return fail(fmt::format("Standalone RTP completion check gave up after {} attempts for generation {}",
                                        MAX_ATTEMPTS, outputLease_.generation));
            }
            if (!eventLoop) {
                return fail("Event loop unavailable during standalone completion check");
            }
            constexpr framenum_t checkStep = RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
            eventLoop->scheduleEvent(std::make_shared<StandaloneRtpCompletionCheckEvent>(
                this->frameNumber + checkStep, outputLease_, span_, skippedFramesTotal_, attempts_ + 1));
            return Result<framenum_t>{this->frameNumber};
        }

        // The lease is released. Check the registry AFTER observing that (the
        // worker publishes a trip's terminal record before releasing) so a
        // breaker termination can't be misread as delivery (issue #97 review).
        if (rtpServer->isGenerationTripped(outputLease_.generation)) {
            return failTerminal();
        }

        // Released without a terminal record: the final frame set was sent (or
        // a newer owner superseded us, which has always counted as a benign
        // outcome for standalone playback).
        if (metrics) {
            metrics->incrementSoundsPlayed();
        }
        if (span_) {
            span_->setAttribute("frames_skipped", static_cast<int64_t>(skippedFramesTotal_));
            span_->setAttribute("rtp.completion.checks", static_cast<int64_t>(attempts_ + 1));
            span_->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

  private:
    Result<framenum_t> failTerminal() {
        if (span_) {
            span_->setAttribute("rtp.work.outcome", "circuit_open");
        }
        return fail(fmt::format("Standalone RTP playback failed: {}",
                                rtpServer->terminalFailureMessage(outputLease_.generation)));
    }

    Result<framenum_t> fail(const std::string &message) {
        if (rtpServer) {
            // No-op if the worker or breaker already released this generation.
            rtpServer->releaseOutput(outputLease_);
        }
        error("{}", message);
        if (span_) {
            span_->setError(message);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, message)};
    }

    rtp::RtpOutputLease outputLease_;
    std::shared_ptr<OperationSpan> span_;
    size_t skippedFramesTotal_;
    uint32_t attempts_;
};

class StandaloneRtpFrameEvent : public EventBase<StandaloneRtpFrameEvent> {
  public:
    StandaloneRtpFrameEvent(framenum_t frameNumber, std::shared_ptr<rtp::AudioStreamBuffer> buffer, size_t frameIndex,
                            rtp::RtpOutputLease outputLease, std::shared_ptr<OperationSpan> span,
                            std::shared_ptr<rtp::StandaloneRtpAdmission::Reservation> reservation,
                            rtp::AsyncAudioTraceContext traceContext, size_t skippedFramesTotal = 0)
        : EventBase(frameNumber), buffer_(std::move(buffer)), frameIndex_(frameIndex),
          outputLease_(std::move(outputLease)), span_(std::move(span)), reservation_(std::move(reservation)),
          traceContext_(std::move(traceContext)), skippedFramesTotal_(skippedFramesTotal) {}

    Result<framenum_t> executeImpl() {
        if (!eventLoop || !rtpServer || !rtpServer->isReady() || !buffer_) {
            return fail("Standalone RTP dispatch dependencies are unavailable");
        }

        // A breaker-terminated generation must not be mistaken for the benign
        // "superseded by a newer owner" case below (issue #97). The registry is
        // re-checked after observing a released lease because the worker
        // publishes the terminal record before releasing.
        if (rtpServer->isGenerationTripped(outputLease_.generation)) {
            return failTerminal();
        }

        if (!rtpServer->isCurrentOutput(outputLease_)) {
            if (rtpServer->isGenerationTripped(outputLease_.generation)) {
                return failTerminal();
            }
            if (span_) {
                span_->setAttribute("rtp.work.outcome", "stale");
                span_->setSuccess();
            }
            return Result<framenum_t>{this->frameNumber};
        }

        constexpr framenum_t dispatchStep = RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
        size_t framesToSkip = 0;
        const framenum_t currentFrame = eventLoop->getCurrentFrameNumber();
        if (currentFrame > this->frameNumber) {
            framesToSkip = static_cast<size_t>((currentFrame - this->frameNumber) / dispatchStep);
            framesToSkip = std::min(framesToSkip, buffer_->getFrameCount() - frameIndex_);
        }

        const size_t dispatchIndex = frameIndex_ + framesToSkip;
        skippedFramesTotal_ += framesToSkip;
        if (dispatchIndex >= buffer_->getFrameCount()) {
            // The tail was skipped for lateness, but earlier frame sets may
            // still be in flight — let the completion check decide the outcome
            // instead of declaring success at enqueue time (issue #97 review).
            rtpServer->releaseOutput(outputLease_);
            eventLoop->scheduleEvent(std::make_shared<StandaloneRtpCompletionCheckEvent>(
                this->frameNumber + dispatchStep, outputLease_, span_, skippedFramesTotal_));
            return Result<framenum_t>{this->frameNumber};
        }

        const bool isFinalFrame = dispatchIndex + 1 >= buffer_->getFrameCount();
        traceContext_.enqueueFrame = currentFrame;
        const auto enqueueResult =
            rtpServer->enqueueAudioFrame(outputLease_, buffer_, dispatchIndex, framesToSkip, isFinalFrame,
                                         isFinalFrame ? reservation_ : nullptr, traceContext_);
        if (enqueueResult == rtp::RtpEnqueueResult::StaleLease) {
            if (span_) {
                span_->setAttribute("rtp.work.outcome", "stale");
                span_->setSuccess();
            }
            return Result<framenum_t>{this->frameNumber};
        }
        if (enqueueResult != rtp::RtpEnqueueResult::Accepted) {
            return fail(fmt::format("Standalone RTP output queue rejected frame {} with result {}", dispatchIndex,
                                    static_cast<int>(enqueueResult)));
        }

        if (metrics) {
            metrics->incrementRtpEventsProcessed();
        }

        if (isFinalFrame) {
            // "Enqueued" is not "delivered". Success is reported by a
            // completion check that waits for the worker to release the lease
            // after actually sending the final frame set (issue #97).
            eventLoop->scheduleEvent(std::make_shared<StandaloneRtpCompletionCheckEvent>(
                this->frameNumber + dispatchStep, outputLease_, span_, skippedFramesTotal_));
            return Result<framenum_t>{this->frameNumber};
        }

        const framenum_t nextFrame = this->frameNumber + static_cast<framenum_t>(framesToSkip + 1) * dispatchStep;
        eventLoop->scheduleEvent(std::make_shared<StandaloneRtpFrameEvent>(nextFrame, buffer_, dispatchIndex + 1,
                                                                           outputLease_, span_, reservation_,
                                                                           traceContext_, skippedFramesTotal_));
        return Result<framenum_t>{this->frameNumber};
    }

  private:
    Result<framenum_t> failTerminal() {
        if (span_) {
            span_->setAttribute("rtp.work.outcome", "circuit_open");
        }
        return fail(fmt::format("Standalone RTP playback failed: {}",
                                rtpServer->terminalFailureMessage(outputLease_.generation)));
    }

    Result<framenum_t> fail(const std::string &message) {
        if (rtpServer) {
            rtpServer->releaseOutput(outputLease_);
        }
        error("{}", message);
        if (span_) {
            span_->setError(message);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, message)};
    }

    std::shared_ptr<rtp::AudioStreamBuffer> buffer_;
    size_t frameIndex_;
    rtp::RtpOutputLease outputLease_;
    std::shared_ptr<OperationSpan> span_;
    std::shared_ptr<rtp::StandaloneRtpAdmission::Reservation> reservation_;
    rtp::AsyncAudioTraceContext traceContext_;
    size_t skippedFramesTotal_;
};

void failAudioSpan(const std::shared_ptr<OperationSpan> &span, const std::string &errorType,
                   const std::string &errorCode, const std::string &errorMessage, const std::string &failureStage,
                   const std::exception *exception = nullptr) {
    if (!span) {
        return;
    }
    span->setAttribute("error.type", errorType);
    span->setAttribute("error.code", errorCode);
    span->setAttribute("error.message", errorMessage);
    span->setAttribute("audio.local.failure_stage", failureStage);
    if (exception) {
        span->recordException(*exception);
    }
    span->setError(errorMessage);
}

} // namespace

// ───── MusicEvent impl ───────────────────────────────────────────────────
MusicEvent::MusicEvent(const framenum_t frameNumber_, std::string filePath_,
                       std::shared_ptr<rtp::StandaloneRtpAdmission::Reservation> rtpReservation,
                       std::string triggerTraceId, std::string triggerSpanId)
    : EventBase(frameNumber_), filePath(std::move(filePath_)), rtpReservation_(std::move(rtpReservation)),
      triggerTraceId_(std::move(triggerTraceId)), triggerSpanId_(std::move(triggerSpanId)) {}

Result<framenum_t> MusicEvent::executeImpl() {
    // Create an observability span if observability is available
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        span = observability->createOperationSpan("music_event.execute");
        span->setAttribute("audio.file.name", std::filesystem::path(filePath).filename().string());
        if (!triggerTraceId_.empty()) {
            span->setAttribute("trigger.trace_id", triggerTraceId_);
        }
        if (!triggerSpanId_.empty()) {
            span->setAttribute("trigger.span_id", triggerSpanId_);
        }
    }

    // Make sure the file exists and is readable
    if (filePath.empty()) {
        std::string errorMessage = "MusicEvent: empty file path provided";
        error(errorMessage);
        failAudioSpan(span, "InvalidAudioPath", "music_event.empty_path", errorMessage, "validation");
        return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    if (!std::filesystem::exists(filePath)) {
        std::string errorMessage = fmt::format("MusicEvent: file doesn't exist '{}'", filePath);
        error(errorMessage);
        failAudioSpan(span, "AudioFileNotFound", "music_event.file_not_found", errorMessage, "validation");
        return Result<framenum_t>{ServerError(ServerError::NotFound, errorMessage)};
    }

    if (!std::filesystem::is_regular_file(filePath)) {
        std::string errorMessage = fmt::format("MusicEvent: not a regular file '{}'", filePath);
        error(errorMessage);
        failAudioSpan(span, "InvalidAudioFile", "music_event.not_regular_file", errorMessage, "validation");
        return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    // Test readability
    if (std::ifstream test(filePath); !test.good()) {
        std::string errorMessage = fmt::format("MusicEvent: unreadable file '{}'", filePath);
        error(errorMessage);
        failAudioSpan(span, "UnreadableAudioFile", "music_event.file_unreadable", errorMessage, "validation");
        return Result<framenum_t>{ServerError(ServerError::Forbidden, errorMessage)};
    }

    if (!config) {
        std::string errorMessage = "MusicEvent: configuration unavailable";
        error(errorMessage);
        failAudioSpan(span, "AudioConfigurationUnavailable", "music_event.config_unavailable", errorMessage,
                      "validation");
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    // Dispatch based on audio mode
    Result result = {this->frameNumber}; // Default to current frame number
    if (config->getAudioMode() == Configuration::AudioMode::RTP) {
        result = scheduleRtpAudio(span);
    } else {
        result = playLocalAudio(span);
    }

    if (result.isSuccess()) {
        if (span)
            span->setSuccess();
        debug("MusicEvent completed successfully");
    } else {
        failAudioSpan(span, "MusicEventError", "music_event.dispatch_failed", result.getError()->getMessage(),
                      "dispatch");
        warn("MusicEvent stumbled: {}", result.getError()->getMessage());
    }

    return result;
}

// ───── Local native playback ─────────────────────────────────────────────
Result<framenum_t> MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan) {
    const bool travelMode = config->getTravelMode();
    const auto result = submitLocalAudio(filePath, travelMode, parentSpan, triggerTraceId_, triggerSpanId_);
    if (!result.isSuccess()) {
        return Result<framenum_t>{result.getError().value()};
    }
    return Result{this->frameNumber};
}

Result<uint64_t> MusicEvent::submitLocalAudio(const std::string &localFilePath, bool travelMode,
                                              std::shared_ptr<OperationSpan> parentSpan,
                                              const std::string &triggerTraceId, const std::string &triggerSpanId) {
    std::shared_ptr<OperationSpan> admissionSpan;
    if (observability) {
        admissionSpan = parentSpan ? observability->createChildOperationSpan("audio.local.admission", parentSpan)
                                   : observability->createOperationSpan("audio.local.admission");
    }

    const auto fileName = std::filesystem::path(localFilePath).filename().string();
    const std::string traceId =
        !triggerTraceId.empty() ? triggerTraceId : (parentSpan ? parentSpan->getTraceIdHex() : std::string{});
    const std::string spanId =
        !triggerSpanId.empty() ? triggerSpanId : (parentSpan ? parentSpan->getSpanIdHex() : std::string{});
    if (admissionSpan) {
        admissionSpan->setAttribute("audio.file.name", fileName);
        admissionSpan->setAttribute("audio.local.source", "standalone");
        admissionSpan->setAttribute("audio.local.mode", travelMode ? "travel" : "main");
        admissionSpan->setAttribute("audio.local.backend", audio::nativeAudioBackendName());
        admissionSpan->setAttribute("audio.device.name", config->getSoundDeviceName().value_or("default"));
        admissionSpan->setAttribute("audio.output.sample_rate_hz", static_cast<int64_t>(config->getSoundFrequency()));
        admissionSpan->setAttribute("audio.output.channels", static_cast<int64_t>(config->getSoundChannels()));
        admissionSpan->setAttribute("audio.local.queue_capacity", static_cast<int64_t>(1));
        if (!traceId.empty()) {
            admissionSpan->setAttribute("trigger.trace_id", traceId);
        }
        if (!spanId.empty()) {
            admissionSpan->setAttribute("trigger.span_id", spanId);
        }
    }

    if (!localAudioPlaybackCoordinator) {
        const std::string errorMessage = "Local audio coordinator unavailable";
        failAudioSpan(admissionSpan, "AudioCoordinatorUnavailable", "local_audio.coordinator_unavailable", errorMessage,
                      "admission");
        if (admissionSpan) {
            admissionSpan->setAttribute("audio.local.outcome", "rejected");
            admissionSpan->end();
        }
        return Result<uint64_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    audio::LocalAudioPlaybackCoordinator::Submission submission;
    try {
        submission = localAudioPlaybackCoordinator->submit(
            {.id = "standalone:" + fileName,
             .source = "standalone",
             .mode = travelMode ? "travel" : "main",
             .backend = audio::nativeAudioBackendName(),
             .deviceName = config->getSoundDeviceName().value_or("default"),
             .outputSampleRate = config->getSoundFrequency(),
             .outputChannels = config->getSoundChannels(),
             .fileName = fileName,
             .triggerTraceId = traceId,
             .triggerSpanId = spanId,
             .play = [localPath = localFilePath, travelMode](const std::atomic<bool> &stopRequested) {
                 const auto mode = travelMode ? audio::NativePlaybackMode::Travel : audio::NativePlaybackMode::Main;
                 return LocalNativeAudioTransport::playFileBlocking(localPath, mode, stopRequested);
             }});
    } catch (const std::exception &exception) {
        const std::string errorMessage = fmt::format("Local audio admission failed: {}", exception.what());
        failAudioSpan(admissionSpan, typeid(exception).name(), "local_audio.admission_exception", errorMessage,
                      "admission", &exception);
        if (admissionSpan) {
            admissionSpan->setAttribute("audio.local.outcome", "rejected");
            admissionSpan->end();
        }
        return Result<uint64_t>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        const std::string errorMessage = "Local audio admission failed with an unknown exception";
        failAudioSpan(admissionSpan, "UnknownAdmissionException", "local_audio.admission_exception", errorMessage,
                      "admission");
        if (admissionSpan) {
            admissionSpan->setAttribute("audio.local.outcome", "rejected");
            admissionSpan->end();
        }
        return Result<uint64_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    if (submission.result != audio::LocalAudioPlaybackCoordinator::SubmitResult::Accepted) {
        const std::string errorMessage = "Local audio coordinator is shutting down";
        failAudioSpan(admissionSpan, "AudioAdmissionRejected", "local_audio.admission_rejected", errorMessage,
                      "admission");
        if (admissionSpan) {
            admissionSpan->setAttribute("audio.local.outcome", "rejected");
            admissionSpan->end();
        }
        return Result<uint64_t>{ServerError(ServerError::Conflict, errorMessage)};
    }

    const uint64_t generation = submission.handle->generation();
    if (admissionSpan) {
        admissionSpan->setAttribute("audio.local.generation", static_cast<int64_t>(generation));
        admissionSpan->setAttribute("audio.local.outcome", "accepted");
        admissionSpan->setSuccess();
        admissionSpan->end();
    }
    return Result<uint64_t>{generation};
}

// ───── RTP / Opus streaming with proper Result handling ──────────────────
Result<framenum_t> MusicEvent::scheduleRtpAudio(std::shared_ptr<OperationSpan> parentSpan) {
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        if (!parentSpan) {
            parentSpan = observability->createOperationSpan("music_event");
        }
        span = observability->createChildOperationSpan("music_event.schedule_rtp", parentSpan);
    }

    if (!eventLoop) {
        std::string errorMsg = "MusicEvent: event loop unavailable";
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Validate RTP server availability
    if (!rtpServer || !rtpServer->isReady()) {
        std::string errorMsg = "RTP server not ready - cannot stream audio";
        error(errorMsg);
        if (span) {
            span->setError(errorMsg);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    debug("RTP server is ready, preparing audio stream for: {}", filePath);

    // Capture the data we need by value so the MusicEvent can die
    const std::string localPath = filePath;
    const framenum_t startingFrame = eventLoop->getNextFrameNumber() + 1;
    auto capturedEventLoop = eventLoop;
    auto capturedRtpServer = rtpServer;
    auto capturedObservability = observability;
    auto reservation = std::move(rtpReservation_);
    if (!reservation) {
        reservation = rtp::standaloneRtpAdmission().tryAcquire();
    }
    if (!reservation) {
        const std::string errorMsg = "Standalone RTP audio loader is busy";
        if (span) {
            span->setError(errorMsg);
        }
        return Result<framenum_t>{ServerError(ServerError::Conflict, errorMsg)};
    }
    if (span)
        span->setAttribute("original_frame_number", startingFrame);

    try {
        std::thread worker([span, localPath, startingFrame, capturedEventLoop, capturedRtpServer, capturedObservability,
                            reservation = std::move(reservation)]() mutable {
            std::optional<rtp::RtpOutputLease> outputLease;
            try {
                debug("RTP worker thread starting");

                if (!capturedEventLoop) {
                    throw std::runtime_error("MusicEvent: event loop unavailable in RTP worker");
                }
                if (!capturedRtpServer || !capturedRtpServer->isReady()) {
                    throw std::runtime_error("RTP server not ready in RTP worker");
                }

                // Heavy I/O – off the event-loop thread.
                debug("Loading audio buffer from WAV file: {}", localPath);
                std::shared_ptr<OperationSpan> encodingSpan;
                if (capturedObservability && span) {
                    encodingSpan = capturedObservability->createChildOperationSpan("music_event.encode_to_opus", span);
                }

                auto buffer = rtp::AudioStreamBuffer::loadFromWavFile(localPath, span);
                if (!buffer) {
                    const auto message = fmt::format("Failed to load audio buffer from '{}'", localPath);
                    if (encodingSpan) {
                        encodingSpan->setError(message);
                    }
                    throw std::runtime_error(message);
                }
                if (encodingSpan) {
                    encodingSpan->setSuccess();
                }

                if (buffer->getFrameCount() == 0) {
                    throw std::runtime_error("Decoded RTP audio buffer contains no frames");
                }

                outputLease = capturedRtpServer->acquireOutput("standalone:" +
                                                               std::filesystem::path(localPath).filename().string());
                rtp::AsyncAudioTraceContext traceContext;
                traceContext.soundFile = std::filesystem::path(localPath).filename().string();
                if (span) {
                    traceContext.triggerTraceId = span->getTraceIdHex();
                    traceContext.triggerSpanId = span->getSpanIdHex();
                }
                if (span) {
                    span->setAttribute("rtp.generation", static_cast<int64_t>(outputLease->generation));
                    span->setAttribute("rtp.owner_id", outputLease->ownerId);
                }

                // Encoding may take time, so establish the playback frame only
                // after the immutable packet buffer is ready.
                const framenum_t streamingStartFrame =
                    capturedEventLoop->getNextFrameNumber() + 2 + RTP_PRIMING_DURATION_FRAMES;
                if (span) {
                    span->setAttribute("streaming_start_frame", streamingStartFrame);
                    span->setAttribute("frames_total", static_cast<int64_t>(buffer->getFrameCount()));
                }
                debug("Original frame: {}, streaming start frame: {}", startingFrame, streamingStartFrame);

                const framenum_t resetFrame = streamingStartFrame - RTP_PRIMING_DURATION_FRAMES;
                capturedEventLoop->scheduleEvent(
                    std::make_shared<RtpEncoderResetEvent>(resetFrame, *outputLease, traceContext));
                capturedEventLoop->scheduleEvent(std::make_shared<StandaloneRtpFrameEvent>(
                    streamingStartFrame, buffer, 0, *outputLease, span, std::move(reservation), traceContext));

                debug("Scheduled the first of {} self-chaining RTP audio frames", buffer->getFrameCount());
            } catch (const std::exception &exception) {
                if (outputLease && capturedRtpServer) {
                    capturedRtpServer->releaseOutput(*outputLease);
                }
                error("Standalone RTP loader failed: {}", exception.what());
                if (span) {
                    span->setError(exception.what());
                }
            } catch (...) {
                if (outputLease && capturedRtpServer) {
                    capturedRtpServer->releaseOutput(*outputLease);
                }
                const std::string errorMsg = "Standalone RTP loader failed with an unknown exception";
                error(errorMsg);
                if (span) {
                    span->setError(errorMsg);
                }
            }
        });

        debug("Detaching RTP worker thread");
        worker.detach();
    } catch (const std::exception &exception) {
        const auto errorMsg = fmt::format("Unable to start standalone RTP loader: {}", exception.what());
        if (span) {
            span->setError(errorMsg);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Return success immediately - the RTP streaming happens in the background
    return Result<framenum_t>{this->frameNumber};
}

} // namespace creatures
