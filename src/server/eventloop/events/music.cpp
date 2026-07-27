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

#include <SDL.h>
#include <spdlog/spdlog.h>

#include "server/audio/LocalAudioPlaybackCoordinator.h"
#include "server/audio/LocalSdlAudioTransport.h"
#include "server/audio/TravelMonoAudioTransport.h"
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
extern const char *audioDevice;
extern SDL_AudioSpec localAudioDeviceAudioSpec;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<audio::LocalAudioPlaybackCoordinator> localAudioPlaybackCoordinator;

// Use constants from config.h - 17 channels for 16 creatures + BGM

namespace {

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

        if (!rtpServer->isCurrentOutput(outputLease_)) {
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
            rtpServer->releaseOutput(outputLease_);
            if (metrics) {
                metrics->incrementSoundsPlayed();
            }
            if (span_) {
                span_->setAttribute("frames_skipped", static_cast<int64_t>(skippedFramesTotal_));
                span_->setSuccess();
            }
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
            if (metrics) {
                metrics->incrementSoundsPlayed();
            }
            if (span_) {
                span_->setAttribute("frames_skipped", static_cast<int64_t>(skippedFramesTotal_));
                span_->setSuccess();
            }
            return Result<framenum_t>{this->frameNumber};
        }

        const framenum_t nextFrame = this->frameNumber + static_cast<framenum_t>(framesToSkip + 1) * dispatchStep;
        eventLoop->scheduleEvent(std::make_shared<StandaloneRtpFrameEvent>(nextFrame, buffer_, dispatchIndex + 1,
                                                                           outputLease_, span_, reservation_,
                                                                           traceContext_, skippedFramesTotal_));
        return Result<framenum_t>{this->frameNumber};
    }

  private:
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

// ───── Local SDL playback ────────────────────────────────────────────────
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
             .fileName = fileName,
             .triggerTraceId = traceId,
             .triggerSpanId = spanId,
             .play = [localPath = localFilePath, travelMode](const std::atomic<bool> &stopRequested) {
                 if (travelMode) {
                     return TravelMonoAudioTransport::playFileBlocking(localPath, stopRequested);
                 }
                 return LocalSdlAudioTransport::playFileBlocking(localPath, stopRequested);
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

// ───── SDL helpers (unchanged) ────────────────────────────────────────────
int MusicEvent::initSDL() {
    debug("Initializing SDL for audio");
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        error("SDL init failed: {}", SDL_GetError());
        return 0;
    }
    debug("SDL initialized successfully!");
    return 1;
}

int MusicEvent::locateAudioDevice() {
    debug("Locating audio device for local playback");

    localAudioDeviceAudioSpec = {};
    localAudioDeviceAudioSpec.freq = static_cast<int>(config->getSoundFrequency());
    localAudioDeviceAudioSpec.channels = config->getSoundChannels();
    localAudioDeviceAudioSpec.format = AUDIO_F32SYS;
    localAudioDeviceAudioSpec.samples = SOUND_BUFFER_SIZE;

    audioDevice = SDL_GetAudioDeviceName(config->getSoundDevice(), 0);
    if (!audioDevice) {
        error("SDL_GetAudioDeviceName failed: {}", SDL_GetError());
        return 0;
    }

    debug("Using audio device: {}", audioDevice);
    return 1;
}

void MusicEvent::listAudioDevices() {
    int n = SDL_GetNumAudioDevices(0);
    debug("SDL reports {} audio devices available:", n);
    for (int i = 0; i < n; ++i) {
        const char *deviceName = SDL_GetAudioDeviceName(i, 0);
        debug("  [{}] {}", i, deviceName ? deviceName : "Unknown");
    }
}

} // namespace creatures
