/**
 * @file MultiOpusRtpServer.h
 * @brief Multi-channel Opus RTP streaming server
 *
 * This file provides a server that can stream multiple channels of Opus-encoded
 * audio over RTP multicast streams with independent SSRC management.
 */

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <uvgrtp/lib.hh>
#include <vector>

#include "server/config.h"
#include "server/rtp/AsyncAudioTraceContext.h"
#include "server/rtp/BoundedCommandQueue.h"
#include "server/rtp/RtcpSender.h"
#include "server/rtp/RtpFrameClock.h"
#include "server/rtp/RtpOutputCoordinator.h"
#include "server/rtp/RtpOutputHealth.h"

namespace creatures {
class OperationSpan;
}

namespace creatures::rtp {

class AudioStreamBuffer;

enum class RtpEnqueueResult {
    Accepted,
    StaleLease,
    QueueFull,
    ServerNotReady,
    InvalidData,
};

class MultiOpusRtpServer {
  public:
    static constexpr size_t OUTPUT_QUEUE_CAPACITY = 64;

    MultiOpusRtpServer();
    ~MultiOpusRtpServer();

    /**
     * Claim exclusive ownership of the process-wide RTP output.
     *
     * The returned generation invalidates every command from the prior owner.
     */
    [[nodiscard]] RtpOutputLease acquireOutput(std::string ownerId);
    void releaseOutput(const RtpOutputLease &lease);
    [[nodiscard]] bool isCurrentOutput(const RtpOutputLease &lease) const;

    // Non-blocking event-loop-to-output-worker commands.
    [[nodiscard]] RtpEnqueueResult enqueueReset(const RtpOutputLease &lease, AsyncAudioTraceContext traceContext = {});
    [[nodiscard]] RtpEnqueueResult enqueueSilentFrame(const RtpOutputLease &lease, size_t primingFrameIndex,
                                                      AsyncAudioTraceContext traceContext = {});
    [[nodiscard]] RtpEnqueueResult enqueueAudioFrame(const RtpOutputLease &lease,
                                                     std::shared_ptr<AudioStreamBuffer> buffer, size_t frameIndex,
                                                     size_t skippedFrames = 0, bool releaseAfterSend = false,
                                                     std::shared_ptr<void> releaseAfterSendHold = nullptr,
                                                     AsyncAudioTraceContext traceContext = {});

    [[nodiscard]] bool isReady() const { return isServerReady_.load(); }
    [[nodiscard]] size_t getPendingCommandCount() const { return outputQueue_.size(); }
    [[nodiscard]] size_t getOutputQueueCapacity() const { return outputQueue_.capacity(); }

    /**
     * Did the send-failure circuit breaker terminate this generation?
     *
     * The breaker releases a tripped generation's lease, which is otherwise
     * indistinguishable from a benign supersede by a newer owner. Callers on
     * the 1 ms event loop use this (a few relaxed atomic loads, no lock) to
     * tell the two apart — including after the lease is gone.
     */
    [[nodiscard]] bool isGenerationTripped(uint64_t generation) const noexcept {
        return terminalFailures_.isTripped(generation);
    }

    /** Details of a breaker-terminated generation, if it tripped. */
    [[nodiscard]] std::optional<TerminalFailure> terminalFailureFor(uint64_t generation) const {
        return terminalFailures_.find(generation);
    }

    /**
     * One-line human-readable description of a breaker-terminated generation,
     * shared by every reporting site so the wording cannot drift.
     */
    [[nodiscard]] std::string terminalFailureMessage(uint64_t generation) const;

  private:
    enum class OutputCommandType {
        Reset,
        SilentFrame,
        AudioFrame,
    };

    struct OutputCommand {
        OutputCommandType type;
        RtpOutputLease lease;
        std::shared_ptr<AudioStreamBuffer> buffer;
        size_t frameIndex{0};
        size_t skippedFrames{0};
        bool releaseAfterSend{false};
        std::shared_ptr<void> releaseAfterSendHold;
        AsyncAudioTraceContext traceContext;
    };

    struct OutputResult {
        rtp_error_t error{RTP_OK};
        uint8_t firstFailedChannel{0};
        uint32_t timestamp{0};
        RtcpSender::SentOctets sentOctets{};
    };

    void runOutputWorker();
    void processOutputCommand(const OutputCommand &command);
    void handleSendOutcome(const OutputCommand &command, const OutputResult *result,
                           const std::exception *exception) noexcept;
    void handleCommandException(const OutputCommand &command, const std::exception &exception) noexcept;
    void recordOutputFailure(const OutputCommand &command, const OutputResult &result,
                             const RtpSendFailureTracker::Action &action) noexcept;
    void recordOutputException(const OutputCommand &command, const std::exception &exception,
                               const RtpSendFailureTracker::Action &action) noexcept;
    void tripCircuitBreaker(const OutputCommand &command, const OutputResult *result, const std::exception *exception,
                            const RtpSendFailureTracker::Action &action, const char *reason) noexcept;
    void releaseGeneration(const RtpOutputLease &lease) noexcept;
    static void applyTraceContextAttributes(const std::shared_ptr<creatures::OperationSpan> &span,
                                            const AsyncAudioTraceContext &traceContext);
    [[nodiscard]] static const char *commandTypeName(OutputCommandType type);
    void rotateSynchronizationSourceIdentifiers(uint64_t generation, const std::string &ownerId,
                                                const AsyncAudioTraceContext &traceContext);
    [[nodiscard]] RtpClockMapping resetFrameTimestamp();
    rtp_error_t send(uint8_t channelIndex, const std::vector<uint8_t> &opusEncodedFrame, uint32_t timestamp);
    OutputResult sendSilentFrameSet(size_t primingFrameIndex);
    OutputResult sendAudioFrameSet(const AudioStreamBuffer &buffer, size_t frameIndex);

    std::atomic<bool> isServerReady_{false};
    uint32_t nextSynchronizationSourceIdentifier_{1000}; // Start from a round number for easy debugging
    uint32_t currentSynchronizationSourceIdentifier_{0}; // Track current SSRC for logging
    RtpFrameClock frameClock_;
    RtcpSender rtcpSender_;
    std::atomic<uint64_t> readyGeneration_{0};
    RtpOutputCoordinator outputCoordinator_;
    BoundedCommandQueue<OutputCommand> outputQueue_{OUTPUT_QUEUE_CAPACITY};
    RtpSendFailureTracker failureTracker_; // worker-thread only
    RtpTerminalFailureRegistry terminalFailures_;
    std::thread outputThread_;

    uvgrtp::context rtpContext_;
    std::array<uvgrtp::session *, RTP_STREAMING_CHANNELS> rtpSessions_{};
    std::array<uvgrtp::media_stream *, RTP_STREAMING_CHANNELS> mediaStreams_{};

    // Each successive packet advances Opus encoder state. AudioStreamBuffer
    // applies the same silence pre-roll before encoding program frame zero.
    std::array<std::array<std::vector<uint8_t>, RTP_PRIMING_FRAMES>, RTP_STREAMING_CHANNELS> encodedSilentFrames_;
};
} // namespace creatures::rtp
