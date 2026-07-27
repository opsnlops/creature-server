//
// music.cpp
//
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
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

} // namespace

// ───── MusicEvent impl ───────────────────────────────────────────────────
MusicEvent::MusicEvent(const framenum_t frameNumber_, std::string filePath_,
                       std::shared_ptr<rtp::StandaloneRtpAdmission::Reservation> rtpReservation)
    : EventBase(frameNumber_), filePath(std::move(filePath_)), rtpReservation_(std::move(rtpReservation)) {}

Result<framenum_t> MusicEvent::executeImpl() {
    // Create an observability span if observability is available
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        span = observability->createOperationSpan("music_event.execute");
        span->setAttribute("file_path", filePath);
    }

    // Make sure the file exists and is readable
    if (filePath.empty()) {
        std::string errorMessage = "MusicEvent: empty file path provided";
        error(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    if (!std::filesystem::exists(filePath)) {
        std::string errorMessage = fmt::format("MusicEvent: file doesn't exist '{}'", filePath);
        error(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::NotFound, errorMessage)};
    }

    if (!std::filesystem::is_regular_file(filePath)) {
        std::string errorMessage = fmt::format("MusicEvent: not a regular file '{}'", filePath);
        error(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    // Test readability
    if (std::ifstream test(filePath); !test.good()) {
        std::string errorMessage = fmt::format("MusicEvent: unreadable file '{}'", filePath);
        error(errorMessage);
        if (span)
            span->setError(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::Forbidden, errorMessage)};
    }

    if (!config) {
        std::string errorMessage = "MusicEvent: configuration unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
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
        if (span)
            span->setError(result.getError()->getMessage());
        warn("MusicEvent stumbled: {}", result.getError()->getMessage());
    }

    return result;
}

// ───── Local SDL playback ────────────────────────────────────────────────
Result<framenum_t> MusicEvent::playLocalAudio(std::shared_ptr<OperationSpan> parentSpan) {
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        if (!parentSpan && observability) {
            parentSpan = observability->createOperationSpan("music_event");
        }
        // Playback may outlive the MusicEvent operation. Use an independent
        // lifecycle span and retain the trigger IDs for cross-system queries.
        span = observability->createOperationSpan("audio.local.playback");
        span->setAttribute("audio.local.file_name", std::filesystem::path(filePath).filename().string());
        span->setAttribute("audio.local.source", "standalone");
        if (parentSpan) {
            span->setAttribute("trigger.trace_id", parentSpan->getTraceIdHex());
            span->setAttribute("trigger.span_id", parentSpan->getSpanIdHex());
        }
    }

    debug("Starting local audio playback for: {}", filePath);

    if (!localAudioPlaybackCoordinator) {
        const std::string errorMessage = "MusicEvent: local audio coordinator unavailable";
        if (span) {
            span->setAttribute("error.code", "local_audio.coordinator_unavailable");
            span->setAttribute("audio.local.failure_stage", "admission");
            span->setError(errorMessage);
            span->end();
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    const bool travelMode = config->getTravelMode();
    const auto fileName = std::filesystem::path(filePath).filename().string();
    auto submission = localAudioPlaybackCoordinator->submit(
        {.id = fmt::format("standalone:{}:{}", fileName, frameNumber),
         .source = "standalone",
         .fileName = fileName,
         .play =
             [localPath = filePath, travelMode](const std::atomic<bool> &stopRequested) {
                 if (travelMode) {
                     return TravelMonoAudioTransport::playFileBlocking(localPath, stopRequested);
                 }
                 return LocalSdlAudioTransport::playFileBlocking(localPath, stopRequested);
             },
         .onFinished =
             [span](const audio::LocalAudioPlaybackCoordinator::Completion &completion) {
                 if (!span) {
                     return;
                 }
                 span->setAttribute("audio.local.generation", static_cast<int64_t>(completion.generation));
                 span->setAttribute("audio.local.source", completion.source);
                 span->setAttribute("audio.local.file_name", completion.fileName);
                 span->setAttribute("audio.local.outcome",
                                    audio::LocalAudioPlaybackCoordinator::outcomeName(completion.result.outcome));
                 if (completion.result.outcome == audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::Failed ||
                     completion.result.outcome == audio::LocalAudioPlaybackCoordinator::PlaybackOutcome::TimedOut) {
                     span->setAttribute("error.code", completion.result.errorCode);
                     span->setAttribute("error.stage", "playback");
                     span->setAttribute("error.message", completion.result.errorMessage);
                     span->setAttribute("audio.local.failure_stage", "playback");
                     span->setError(completion.result.errorMessage);
                 } else {
                     span->setSuccess();
                 }
                 span->end();
             }});
    if (submission.result != audio::LocalAudioPlaybackCoordinator::SubmitResult::Accepted) {
        const std::string errorMessage = "MusicEvent: local audio coordinator is shutting down";
        if (span) {
            span->setAttribute("error.code", "local_audio.admission_rejected");
            span->setAttribute("audio.local.failure_stage", "admission");
            span->setError(errorMessage);
            span->end();
        }
        return Result<framenum_t>{ServerError(ServerError::Conflict, errorMessage)};
    }

    if (span) {
        span->setAttribute("audio.local.generation", static_cast<int64_t>(submission.handle->generation()));
        span->setAttribute("audio.local.mode", travelMode ? "travel" : "main");
        span->setAttribute("audio.local.queue_capacity", static_cast<int64_t>(1));
    }

    // Return immediately - the music plays in the background
    return Result{this->frameNumber};
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
