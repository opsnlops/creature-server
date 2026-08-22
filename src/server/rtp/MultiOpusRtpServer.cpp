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
#include "util/websocketUtils.h"

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
    size_t purged = 0;
    {
        auto guard = outputCoordinator_.lockIfCurrent(lease);
        if (guard) {
            readyGeneration_.store(0);
            rtcpSender_.discardSessionUnless(lease.generation);
            purged = outputQueue_.eraseIf(
                [&lease](const OutputCommand &command) { return command.lease.generation != lease.generation; });
        }
    }
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
        } catch (...) {
            // processOutputCommand handles its own exceptions (including the
            // publish-before-release ordering); this is a last-resort guard so
            // the worker thread can never die.
            error("Unexpected exception escaped RTP output command processing for {} generation {}",
                  command->lease.ownerId, command->lease.generation);
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
                failureTracker_.beginGeneration(command.lease.generation, std::chrono::steady_clock::now());
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

    } catch (const std::exception &exception) {
        handleCommandException(command, exception);
        return;
    } catch (...) {
        const std::runtime_error exception("unknown RTP output exception");
        handleCommandException(command, exception);
        return;
    }

    // The outcome — and any terminal-record publication it triggers — must
    // precede the final-frame release: a reader that observes a released lease
    // must always be able to find its terminal record. (Both run after the
    // guard scope; the coordinator mutex is non-recursive.)
    if (command.type != OutputCommandType::Reset) {
        handleSendOutcome(command, &result, nullptr);
    }
    if (command.releaseAfterSend) {
        releaseGeneration(command.lease);
    }
}

/**
 * Exception funnel for a command that threw (issue #97).
 *
 * A throwing Reset never attempted a send, so it bypasses the send-failure
 * tracker — but its generation is terminal immediately: the lease must be
 * released, and without a terminal record every reader would misread that
 * release as a benign supersede. Send-command exceptions feed the breaker
 * like any failed send, then honor the final-frame release contract.
 */
void MultiOpusRtpServer::handleCommandException(const OutputCommand &command,
                                                const std::exception &exception) noexcept {
    try {
        if (command.type == OutputCommandType::Reset) {
            const RtpSendFailureTracker::Action action{};
            recordOutputException(command, exception, action);
            tripCircuitBreaker(command, nullptr, &exception, action, "reset_failed");
            return;
        }

        handleSendOutcome(command, nullptr, &exception);
        if (command.releaseAfterSend) {
            // No-op if the breaker already released this generation.
            releaseGeneration(command.lease);
        }
    } catch (const std::exception &handlerException) {
        error("Failed to handle RTP command exception: {}", handlerException.what());
    } catch (...) {
        error("Failed to handle RTP command exception");
    }
}

/** The worker's release idiom: every step is generation-guarded, so releasing
 *  an already-released or superseded generation is a safe no-op. */
void MultiOpusRtpServer::releaseGeneration(const RtpOutputLease &lease) noexcept {
    outputCoordinator_.release(lease);
    rtcpSender_.endSession(lease.generation);
    uint64_t expectedGeneration = lease.generation;
    readyGeneration_.compare_exchange_strong(expectedGeneration, 0);
}

/**
 * Single funnel for every frame-set send outcome (issue #97).
 *
 * Success clears the failure run (emitting a recovery signal if one was
 * active). Failure feeds the circuit breaker: the raw send-failure counter
 * stays authoritative on every failure, while detailed logs/spans are gated to
 * the first failure of a run and every Nth thereafter. Crossing a trip
 * threshold terminates the generation.
 */
void MultiOpusRtpServer::handleSendOutcome(const OutputCommand &command, const OutputResult *result,
                                           const std::exception *exception) noexcept {
    try {
        const auto now = std::chrono::steady_clock::now();

        if (!exception && result && result->error == RTP_OK) {
            const auto action = failureTracker_.recordSuccess(command.lease.generation, now);
            if (action.recovered) {
                info("RTP output sends recovered for {} generation {} after {} consecutive failure(s)",
                     command.lease.ownerId, command.lease.generation, action.consecutiveFailures);
                if (creatures::metrics) {
                    creatures::metrics->incrementRtpSendRecoveries();
                }
            }
            return;
        }

        const auto action = failureTracker_.recordFailure(command.lease.generation, now);
        if (creatures::metrics) {
            creatures::metrics->incrementRtpSendFailures();
        }
        if (action.emitDetail) {
            if (exception) {
                recordOutputException(command, *exception, action);
            } else if (result) {
                recordOutputFailure(command, *result, action);
            }
        } else if (creatures::metrics) {
            creatures::metrics->incrementRtpSendFailuresSuppressed();
        }
        if (action.trip) {
            tripCircuitBreaker(command, result, exception, action, "failure_threshold");
        } else if (command.releaseAfterSend && action.consecutiveFailures >= 2) {
            // The stream's FINAL frame set failed as part of a failure run: no
            // further send will retry it and the lease is about to be released,
            // so end the generation terminally even though no threshold
            // tripped. An isolated single-frame blip on the last frame stays a
            // delivered-with-loss success (April's ≥2 rule, 2026-08-21).
            tripCircuitBreaker(command, result, exception, action, "final_frame_failure");
        }
    } catch (const std::exception &handlerException) {
        error("Failed to handle RTP send outcome: {}", handlerException.what());
    } catch (...) {
        error("Failed to handle RTP send outcome");
    }
}

/**
 * Open the circuit for a generation whose sends keep failing: publish the
 * terminal record (so the event loop can distinguish this from a benign
 * supersede even after the lease is gone), release the generation, drop its
 * queued frames, and tell connected clients — the same notice broadcast the
 * shutdown path uses.
 */
void MultiOpusRtpServer::tripCircuitBreaker(const OutputCommand &command, const OutputResult *result,
                                            const std::exception *exception,
                                            const RtpSendFailureTracker::Action &action, const char *reason) noexcept {
    try {
        TerminalFailure failure;
        failure.generation = command.lease.generation;
        failure.ownerId = command.lease.ownerId;
        failure.commandType = commandTypeName(command.type);
        failure.frameIndex = command.frameIndex;
        failure.consecutiveFailures = action.consecutiveFailures;
        failure.traceContext = command.traceContext;
        if (result) {
            failure.errorCode = static_cast<int>(result->error);
            failure.firstFailedChannel = result->firstFailedChannel;
            failure.rtpTimestamp = result->timestamp;
            failure.errorMessage = fmt::format("RTP send failed on channel {} with error {}",
                                               result->firstFailedChannel, static_cast<int>(result->error));
        } else if (exception) {
            failure.errorMessage = exception->what();
        }

        // Publish before releasing anything: a reader must never observe the
        // released lease without the terminal record being findable.
        terminalFailures_.publish(failure);

        releaseGeneration(command.lease);
        const size_t purged = outputQueue_.eraseIf(
            [&command](const OutputCommand &queued) { return queued.lease.generation == command.lease.generation; });

        error("RTP circuit breaker OPEN ({}) for {} generation {}: {} ({} consecutive / {} windowed failures, {} "
              "queued command(s) dropped)",
              reason, command.lease.ownerId, command.lease.generation, failure.errorMessage, action.consecutiveFailures,
              action.windowedFailures, purged);

        if (creatures::metrics) {
            creatures::metrics->incrementRtpCircuitBreakerTrips();
        }

        if (auto span = creatures::observability
                            ? creatures::observability->createOperationSpan("rtp.output.circuit_open")
                            : nullptr) {
            span->setAttribute("rtp.owner_id", command.lease.ownerId);
            span->setAttribute("rtp.generation", static_cast<int64_t>(command.lease.generation));
            span->setAttribute("rtp.command.type", failure.commandType);
            span->setAttribute("rtp.frame.index", static_cast<int64_t>(failure.frameIndex));
            span->setAttribute("rtp.circuit_open.reason", reason);
            span->setAttribute("rtp.failures.consecutive", static_cast<int64_t>(action.consecutiveFailures));
            span->setAttribute("rtp.failures.windowed", static_cast<int64_t>(action.windowedFailures));
            span->setAttribute("rtp.queue.purged", static_cast<int64_t>(purged));
            applyTraceContextAttributes(span, command.traceContext);
            span->setAttribute("error.type", "RtpCircuitBreakerOpen");
            span->setAttribute("error.code", failure.errorCode);
            span->setAttribute("error.message", failure.errorMessage);
            span->setError(failure.errorMessage);
        }

        // Tell connected clients, the same way the shutdown path does. The
        // broadcast just enqueues onto the websocket outgoing queue, so it is
        // safe from this worker thread.
        const auto broadcastResult = creatures::broadcastNoticeToAllClients(fmt::format(
            "RTP audio output failed and playback was stopped ({} — {})", command.lease.ownerId, failure.errorMessage));
        if (!broadcastResult.isSuccess()) {
            warn("Unable to broadcast RTP circuit breaker notice: {}", broadcastResult.getError()->getMessage());
        }
    } catch (const std::exception &tripException) {
        error("Failed to trip RTP circuit breaker: {}", tripException.what());
    } catch (...) {
        error("Failed to trip RTP circuit breaker");
    }
}

std::string MultiOpusRtpServer::terminalFailureMessage(uint64_t generation) const {
    auto message = fmt::format("the RTP send-failure circuit breaker terminated output generation {}", generation);
    if (const auto failure = terminalFailures_.find(generation)) {
        message = fmt::format("{}: {}", message, failure->errorMessage);
    }
    return message;
}

void MultiOpusRtpServer::applyTraceContextAttributes(const std::shared_ptr<creatures::OperationSpan> &span,
                                                     const AsyncAudioTraceContext &traceContext) {
    if (!span) {
        return;
    }
    if (!traceContext.triggerTraceId.empty()) {
        span->setAttribute("trigger.trace_id", traceContext.triggerTraceId);
        span->setAttribute("trigger.span_id", traceContext.triggerSpanId);
    }
    if (!traceContext.sessionId.empty()) {
        span->setAttribute("session.id", traceContext.sessionId);
    }
    if (!traceContext.animationId.empty()) {
        span->setAttribute("animation.id", traceContext.animationId);
    }
    if (!traceContext.soundFile.empty()) {
        span->setAttribute("sound.file", traceContext.soundFile);
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

void MultiOpusRtpServer::recordOutputFailure(const OutputCommand &command, const OutputResult &result,
                                             const RtpSendFailureTracker::Action &action) noexcept {
    try {
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
        applyTraceContextAttributes(span, command.traceContext);
        span->setAttribute("rtp.failures.consecutive", static_cast<int64_t>(action.consecutiveFailures));
        span->setAttribute("rtp.failures.windowed", static_cast<int64_t>(action.windowedFailures));
        span->setAttribute("rtp.failures.suppressed_since_last", static_cast<int64_t>(action.suppressedSinceLastEmit));
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

void MultiOpusRtpServer::recordOutputException(const OutputCommand &command, const std::exception &exception,
                                               const RtpSendFailureTracker::Action &action) noexcept {
    try {
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
        applyTraceContextAttributes(span, command.traceContext);
        span->setAttribute("rtp.failures.consecutive", static_cast<int64_t>(action.consecutiveFailures));
        span->setAttribute("rtp.failures.windowed", static_cast<int64_t>(action.windowedFailures));
        span->setAttribute("rtp.failures.suppressed_since_last", static_cast<int64_t>(action.suppressedSinceLastEmit));
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
