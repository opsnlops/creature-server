/**
 * @file MultiOpusRtpServer.cpp
 * @brief Implementation of the multi-channel Opus RTP streaming server
 *
 * This file contains the implementation of the MultiOpusRtpServer class
 * which manages multiple RTP streams for Opus-encoded audio channels.
 */

#include <chrono>
#include <exception>
#include <limits>
#include <random>
#include <stdexcept>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <uvgrtp/lib.hh>
#include <uvgrtp/media_stream.hh>
#include <uvgrtp/util.hh>

#include "MultiOpusRtpServer.h"
#include "server/config.h"
#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"
#include "server/rtp/opus/OpusPriming.h"
#include "util/ObservabilityManager.h"
#include "util/threadName.h"

using namespace creatures::rtp;

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace {

uint32_t randomSynchronizationSourceBase() {
    std::random_device randomDevice;
    std::uniform_int_distribution<uint32_t> distribution(1U,
                                                         std::numeric_limits<uint32_t>::max() - RTP_STREAMING_CHANNELS);
    return distribution(randomDevice);
}

} // namespace

MultiOpusRtpServer::MultiOpusRtpServer() {
    try {
        for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            // Create session with multicast address
            rtpSessions_[channelIndex] = rtpContext_.create_session(RTP_GROUPS[channelIndex]);
            if (!rtpSessions_[channelIndex]) {
                throw std::runtime_error(fmt::format("unable to create RTP session for channel {}", channelIndex));
            }

            // Create media stream with local port, remote port, format, and flags
            mediaStreams_[channelIndex] = rtpSessions_[channelIndex]->create_stream(RTP_PORT,        // source port
                                                                                    RTP_PORT,        // destination port
                                                                                    RTP_FORMAT_OPUS, // format
                                                                                    RCE_SEND_ONLY);  // flags
            if (!mediaStreams_[channelIndex]) {
                throw std::runtime_error(fmt::format("unable to create RTP stream for channel {}", channelIndex));
            }

            // Override the dynamic payload type so VLC/Wireshark recognize Opus (payload type 96)
            if (mediaStreams_[channelIndex]->configure_ctx(RCC_DYN_PAYLOAD_TYPE, RTP_OPUS_PAYLOAD_PT) != RTP_OK) {
                throw std::runtime_error(
                    fmt::format("unable to configure RTP payload type for channel {}", channelIndex));
            }

            // Configure the clock rate for accurate timing (48 kHz)
            if (mediaStreams_[channelIndex]->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE) != RTP_OK) {
                throw std::runtime_error(
                    fmt::format("unable to configure RTP clock rate for channel {}", channelIndex));
            }

            // Pre-encode the complete silence sequence. Reusing packet zero
            // would repeatedly present the decoder with the wrong encoder
            // history at stream startup.
            opus::Encoder encoder;
            encodedSilentFrames_[channelIndex] = opus::encodePrimingSequence(encoder);
        }

        // Set initial SSRC values - each channel gets its own sequential SSRC
        rotateSynchronizationSourceIdentifiers(0, "startup", {});
        outputThread_ = std::thread(&MultiOpusRtpServer::runOutputWorker, this);
        isServerReady_.store(true);

        info("MultiOpusRtpServer initialized with {} channels, starting SSRC: {}", RTP_STREAMING_CHANNELS,
             currentSynchronizationSourceIdentifier_);
    } catch (const std::exception &exception) {
        isServerReady_.store(false);
        outputQueue_.stop();
        error("MultiOpusRtpServer initialization failed: {}", exception.what());
    }
}

MultiOpusRtpServer::~MultiOpusRtpServer() {
    isServerReady_.store(false);
    outputQueue_.stop();
    if (outputThread_.joinable()) {
        outputThread_.join();
    }
    rtcpSender_.shutdown();

    for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        if (rtpSessions_[channelIndex] && mediaStreams_[channelIndex]) {
            rtpSessions_[channelIndex]->destroy_stream(mediaStreams_[channelIndex]);
        }
        if (rtpSessions_[channelIndex]) {
            rtpContext_.destroy_session(rtpSessions_[channelIndex]);
        }
    }
}

RtpOutputLease MultiOpusRtpServer::acquireOutput(std::string ownerId) {
    auto lease = outputCoordinator_.acquire(std::move(ownerId));
    readyGeneration_.store(0);
    rtcpSender_.discardSessionUnless(lease.generation);
    const size_t purged = outputQueue_.eraseIf(
        [&lease](const OutputCommand &command) { return command.lease.generation != lease.generation; });
    info("RTP output acquired by {} at generation {} ({} stale command(s) purged)", lease.ownerId, lease.generation,
         purged);
    return lease;
}

void MultiOpusRtpServer::releaseOutput(const RtpOutputLease &lease) {
    outputCoordinator_.release(lease);
    rtcpSender_.endSession(lease.generation);
    uint64_t expectedGeneration = lease.generation;
    readyGeneration_.compare_exchange_strong(expectedGeneration, 0);
    const size_t purged = outputQueue_.eraseIf(
        [&lease](const OutputCommand &command) { return command.lease.generation == lease.generation; });
    debug("RTP output release requested by {} at generation {} ({} queued command(s) purged)", lease.ownerId,
          lease.generation, purged);
}

bool MultiOpusRtpServer::isCurrentOutput(const RtpOutputLease &lease) const {
    return outputCoordinator_.isCurrent(lease);
}

RtpEnqueueResult MultiOpusRtpServer::enqueueReset(const RtpOutputLease &lease, AsyncAudioTraceContext traceContext) {
    if (!isReady()) {
        return RtpEnqueueResult::ServerNotReady;
    }
    if (!outputCoordinator_.isCurrent(lease)) {
        return RtpEnqueueResult::StaleLease;
    }
    if (!outputQueue_.tryPush(
            OutputCommand{OutputCommandType::Reset, lease, nullptr, 0, 0, false, nullptr, std::move(traceContext)})) {
        return RtpEnqueueResult::QueueFull;
    }
    return RtpEnqueueResult::Accepted;
}

RtpEnqueueResult MultiOpusRtpServer::enqueueSilentFrame(const RtpOutputLease &lease, size_t primingFrameIndex,
                                                        AsyncAudioTraceContext traceContext) {
    if (!isReady()) {
        return RtpEnqueueResult::ServerNotReady;
    }
    if (!outputCoordinator_.isCurrent(lease)) {
        return RtpEnqueueResult::StaleLease;
    }
    if (primingFrameIndex >= RTP_PRIMING_FRAMES) {
        return RtpEnqueueResult::InvalidData;
    }
    if (!outputQueue_.tryPush(OutputCommand{OutputCommandType::SilentFrame, lease, nullptr, primingFrameIndex, 0, false,
                                            nullptr, std::move(traceContext)})) {
        return RtpEnqueueResult::QueueFull;
    }
    return RtpEnqueueResult::Accepted;
}

RtpEnqueueResult MultiOpusRtpServer::enqueueAudioFrame(const RtpOutputLease &lease,
                                                       std::shared_ptr<AudioStreamBuffer> buffer, size_t frameIndex,
                                                       size_t skippedFrames, bool releaseAfterSend,
                                                       std::shared_ptr<void> releaseAfterSendHold,
                                                       AsyncAudioTraceContext traceContext) {
    if (!isReady()) {
        return RtpEnqueueResult::ServerNotReady;
    }
    if (!buffer || frameIndex >= buffer->getFrameCount()) {
        return RtpEnqueueResult::InvalidData;
    }
    if (!outputCoordinator_.isCurrent(lease)) {
        return RtpEnqueueResult::StaleLease;
    }
    if (!outputQueue_.tryPush(OutputCommand{OutputCommandType::AudioFrame, lease, std::move(buffer), frameIndex,
                                            skippedFrames, releaseAfterSend, std::move(releaseAfterSendHold),
                                            std::move(traceContext)})) {
        return RtpEnqueueResult::QueueFull;
    }
    return RtpEnqueueResult::Accepted;
}

void MultiOpusRtpServer::runOutputWorker() {
    setThreadName("RtpOutputWorker");
    while (auto command = outputQueue_.waitPop()) {
        try {
            processOutputCommand(*command);
        } catch (const std::exception &exception) {
            recordOutputException(*command, exception);
        } catch (...) {
            const std::runtime_error exception("unknown RTP output exception");
            recordOutputException(*command, exception);
        }
    }
}

void MultiOpusRtpServer::processOutputCommand(const OutputCommand &command) {
    OutputResult result;
    try {
        {
            auto guard = outputCoordinator_.lockIfCurrent(command.lease);
            if (!guard) {
                debug("Discarding stale RTP command for {} generation {}", command.lease.ownerId,
                      command.lease.generation);
                return;
            }

            switch (command.type) {
            case OutputCommandType::Reset:
                readyGeneration_.store(0);
                rotateSynchronizationSourceIdentifiers(command.lease.generation, command.lease.ownerId,
                                                       command.traceContext);
                readyGeneration_.store(command.lease.generation);
                break;
            case OutputCommandType::SilentFrame:
                if (readyGeneration_.load() != command.lease.generation) {
                    throw std::runtime_error("RTP silent frame rejected because its generation was not reset");
                }
                result = sendSilentFrameSet(command.frameIndex);
                rtcpSender_.recordFrame(command.lease.generation, result.sentOctets);
                frameClock_.advance();
                break;
            case OutputCommandType::AudioFrame:
                if (readyGeneration_.load() != command.lease.generation) {
                    throw std::runtime_error("RTP audio frame rejected because its generation was not reset");
                }
                frameClock_.advance(command.skippedFrames);
                result = sendAudioFrameSet(*command.buffer, command.frameIndex);
                rtcpSender_.recordFrame(command.lease.generation, result.sentOctets);
                frameClock_.advance();
                break;
            }
        }

        // Guard's non-recursive coordinator mutex must be released first.
        if (command.releaseAfterSend) {
            outputCoordinator_.release(command.lease);
            rtcpSender_.endSession(command.lease.generation);
            uint64_t expectedGeneration = command.lease.generation;
            readyGeneration_.compare_exchange_strong(expectedGeneration, 0);
        }
    } catch (...) {
        if (command.type == OutputCommandType::Reset || command.releaseAfterSend) {
            outputCoordinator_.release(command.lease);
            rtcpSender_.endSession(command.lease.generation);
            uint64_t expectedGeneration = command.lease.generation;
            readyGeneration_.compare_exchange_strong(expectedGeneration, 0);
        }
        throw;
    }

    if (result.error != RTP_OK) {
        recordOutputFailure(command, result);
    }
}

const char *MultiOpusRtpServer::commandTypeName(OutputCommandType type) {
    switch (type) {
    case OutputCommandType::Reset:
        return "reset";
    case OutputCommandType::SilentFrame:
        return "silent_frame";
    case OutputCommandType::AudioFrame:
        return "audio_frame";
    }
    return "unknown";
}

void MultiOpusRtpServer::recordOutputFailure(const OutputCommand &command, const OutputResult &result) noexcept {
    try {
        if (creatures::metrics) {
            creatures::metrics->incrementRtpSendFailures();
        }

        const auto errorMessage =
            fmt::format("RTP {} send failed on channel {} at timestamp {} with error {}", commandTypeName(command.type),
                        result.firstFailedChannel, result.timestamp, static_cast<int>(result.error));
        error("{} (owner {}, generation {}, frame {})", errorMessage, command.lease.ownerId, command.lease.generation,
              command.frameIndex);

        auto span = creatures::observability ? creatures::observability->createOperationSpan("rtp.output.send_failure")
                                             : nullptr;
        if (!span) {
            return;
        }
        span->setAttribute("rtp.command.type", commandTypeName(command.type));
        span->setAttribute("rtp.owner_id", command.lease.ownerId);
        span->setAttribute("rtp.generation", static_cast<int64_t>(command.lease.generation));
        span->setAttribute("rtp.channel", result.firstFailedChannel);
        span->setAttribute("rtp.timestamp", result.timestamp);
        span->setAttribute("rtp.frame.index", static_cast<int64_t>(command.frameIndex));
        span->setAttribute("rtp.frames.skipped", static_cast<int64_t>(command.skippedFrames));
        span->setAttribute("rtp.enqueue.frame", command.traceContext.enqueueFrame);
        span->setAttribute("rtp.queue.depth", static_cast<int64_t>(outputQueue_.size()));
        span->setAttribute("rtp.queue.capacity", static_cast<int64_t>(outputQueue_.capacity()));
        if (!command.traceContext.triggerTraceId.empty()) {
            span->setAttribute("trigger.trace_id", command.traceContext.triggerTraceId);
            span->setAttribute("trigger.span_id", command.traceContext.triggerSpanId);
        }
        if (!command.traceContext.sessionId.empty()) {
            span->setAttribute("session.id", command.traceContext.sessionId);
        }
        if (!command.traceContext.animationId.empty()) {
            span->setAttribute("animation.id", command.traceContext.animationId);
        }
        if (!command.traceContext.soundFile.empty()) {
            span->setAttribute("sound.file", command.traceContext.soundFile);
        }
        span->setAttribute("error.type", "RtpSendFailure");
        span->setAttribute("error.code", static_cast<int>(result.error));
        span->setAttribute("error.message", errorMessage);
        span->setError(errorMessage);
    } catch (const std::exception &exception) {
        error("Failed to record RTP output failure: {}", exception.what());
    } catch (...) {
        error("Failed to record RTP output failure");
    }
}

void MultiOpusRtpServer::recordOutputException(const OutputCommand &command, const std::exception &exception) noexcept {
    try {
        if (creatures::metrics) {
            creatures::metrics->incrementRtpSendFailures();
        }
        const auto errorMessage =
            fmt::format("RTP {} command threw: {}", commandTypeName(command.type), exception.what());
        error("{} (owner {}, generation {}, frame {})", errorMessage, command.lease.ownerId, command.lease.generation,
              command.frameIndex);

        auto span = creatures::observability
                        ? creatures::observability->createOperationSpan("rtp.output.send_exception")
                        : nullptr;
        if (!span) {
            return;
        }
        span->setAttribute("rtp.command.type", commandTypeName(command.type));
        span->setAttribute("rtp.owner_id", command.lease.ownerId);
        span->setAttribute("rtp.generation", static_cast<int64_t>(command.lease.generation));
        span->setAttribute("rtp.frame.index", static_cast<int64_t>(command.frameIndex));
        span->setAttribute("rtp.frames.skipped", static_cast<int64_t>(command.skippedFrames));
        span->setAttribute("rtp.enqueue.frame", command.traceContext.enqueueFrame);
        if (!command.traceContext.triggerTraceId.empty()) {
            span->setAttribute("trigger.trace_id", command.traceContext.triggerTraceId);
            span->setAttribute("trigger.span_id", command.traceContext.triggerSpanId);
        }
        if (!command.traceContext.sessionId.empty()) {
            span->setAttribute("session.id", command.traceContext.sessionId);
        }
        if (!command.traceContext.animationId.empty()) {
            span->setAttribute("animation.id", command.traceContext.animationId);
        }
        if (!command.traceContext.soundFile.empty()) {
            span->setAttribute("sound.file", command.traceContext.soundFile);
        }
        span->setAttribute("error.type", "RtpOutputException");
        span->setAttribute("error.message", errorMessage);
        span->recordException(exception);
        span->setError(errorMessage);
    } catch (const std::exception &recordingException) {
        error("Failed to record RTP output exception: {}", recordingException.what());
    } catch (...) {
        error("Failed to record RTP output exception");
    }
}

rtp_error_t MultiOpusRtpServer::send(uint8_t channelIndex, const std::vector<uint8_t> &opusEncodedFrame,
                                     uint32_t timestamp) {
    if (channelIndex >= RTP_STREAMING_CHANNELS || !mediaStreams_[channelIndex]) {
        return RTP_INVALID_VALUE;
    }

    return mediaStreams_[channelIndex]->push_frame(
        const_cast<uint8_t *>(opusEncodedFrame.data()), // uvgRTP requires non-const pointer
        opusEncodedFrame.size(), timestamp, RTP_NO_FLAGS);
}

void MultiOpusRtpServer::rotateSynchronizationSourceIdentifiers(uint64_t generation, const std::string &ownerId,
                                                                const AsyncAudioTraceContext &traceContext) {
    nextSynchronizationSourceIdentifier_ = randomSynchronizationSourceBase();
    currentSynchronizationSourceIdentifier_ = nextSynchronizationSourceIdentifier_;
    RtcpSender::SynchronizationSources synchronizationSources{};

    for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        if (mediaStreams_[channelIndex]) {
            // Assign fresh SSRC, then increment for next channel
            const uint32_t synchronizationSource = nextSynchronizationSourceIdentifier_++;
            if (mediaStreams_[channelIndex]->configure_ctx(RCC_SSRC, synchronizationSource) != RTP_OK) {
                throw std::runtime_error(fmt::format("unable to configure RTP SSRC for channel {}", channelIndex));
            }
            synchronizationSources[channelIndex] = synchronizationSource;
        }
    }

    const auto clockMapping = resetFrameTimestamp();
    rtcpSender_.beginSession(generation, ownerId, synchronizationSources, clockMapping, traceContext);
    debug("SSRC rotated! New range: {} to {}", currentSynchronizationSourceIdentifier_,
          nextSynchronizationSourceIdentifier_ - 1);
}

MultiOpusRtpServer::OutputResult MultiOpusRtpServer::sendSilentFrameSet(size_t primingFrameIndex) {
    const uint32_t timestamp = frameClock_.current();
    OutputResult frameResult{RTP_OK, 0, timestamp, {}};

    for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        const auto &encodedFrame = encodedSilentFrames_[channelIndex][primingFrameIndex];
        const auto transmissionResult = send(channelIndex, encodedFrame, timestamp);
        if (transmissionResult != RTP_OK && frameResult.error == RTP_OK) {
            frameResult.error = transmissionResult;
            frameResult.firstFailedChannel = channelIndex;
        } else if (transmissionResult == RTP_OK) {
            frameResult.sentOctets[channelIndex] = static_cast<uint32_t>(encodedFrame.size());
        }
    }
    return frameResult;
}

MultiOpusRtpServer::OutputResult MultiOpusRtpServer::sendAudioFrameSet(const AudioStreamBuffer &buffer,
                                                                       size_t frameIndex) {
    const uint32_t timestamp = frameClock_.current();
    OutputResult frameResult{RTP_OK, 0, timestamp, {}};

    for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        const auto &encodedFrame = buffer.getEncodedFrame(channelIndex, frameIndex);
        const auto transmissionResult = send(channelIndex, encodedFrame, timestamp);
        if (transmissionResult != RTP_OK && frameResult.error == RTP_OK) {
            frameResult.error = transmissionResult;
            frameResult.firstFailedChannel = channelIndex;
        } else if (transmissionResult == RTP_OK) {
            frameResult.sentOctets[channelIndex] = static_cast<uint32_t>(encodedFrame.size());
        }
    }
    return frameResult;
}

RtpClockMapping MultiOpusRtpServer::resetFrameTimestamp() {
    auto clockMapping = RtpClockMapping::capture(RTP_SRATE);
    frameClock_.reset(clockMapping.rtpEpoch());
    return clockMapping;
}
