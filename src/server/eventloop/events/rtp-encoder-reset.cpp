// src/server/eventloop/events/rtp-encoder-reset.cpp

#include <spdlog/spdlog.h>

#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;

RtpEncoderResetEvent::RtpEncoderResetEvent(framenum_t frameNumber_, uint8_t silentFrameCount_)
    : EventBase(frameNumber_), silentFrameCount_(silentFrameCount_) {}

Result<framenum_t> RtpEncoderResetEvent::executeImpl() {
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        span = observability->createOperationSpan("rtp_encoder_reset_event.execute");
        span->setAttribute("frame_number", static_cast<int64_t>(frameNumber));
        span->setAttribute("silent_frame_count", static_cast<int64_t>(silentFrameCount_));
    }

    debug("RtpEncoderResetEvent executing on frame {}", frameNumber);

    try {
        if (!rtpServer || !rtpServer->isReady()) {
            const auto errorMsg = "RTP server not available for encoder reset";
            error(errorMsg);
            if (span)
                span->setError(errorMsg);
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }

        // Step 1: Rotate SSRC for all channels
        const auto oldSSRC = rtpServer->getCurrentSynchronizationSourceIdentifier();
        rtpServer->rotateSynchronizationSourceIdentifiers();
        const auto newSSRC = rtpServer->getCurrentSynchronizationSourceIdentifier();

        debug("SSRC rotated from {} to {} - fresh identity for all channels", oldSSRC, newSSRC);

        // Step 2: Reset all Opus encoders
        rtpServer->resetAllEncoders();
        debug("All encoders reset - clean slate for encoding");

        // Step 3: Prime decoders without blocking this 1ms event-loop tick.
        // Each RtpSilentFrameEvent sends one 10ms sample interval and schedules
        // the next interval independently.
        if (silentFrameCount_ > 0) {
            if (!eventLoop) {
                const auto errorMsg = "Event loop unavailable for RTP decoder priming";
                if (span)
                    span->setError(errorMsg);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }
            eventLoop->scheduleEvent(std::make_shared<RtpSilentFrameEvent>(frameNumber, silentFrameCount_));
            debug("Scheduled {} silent priming frames", silentFrameCount_);
        }

        // Update metrics
        if (metrics) {
            metrics->incrementRtpEncoderResets();
        }
        if (span) {
            span->setAttribute("old_ssrc", static_cast<int64_t>(oldSSRC));
            span->setAttribute("new_ssrc", static_cast<int64_t>(newSSRC));
            span->setSuccess();
        }

        info("RTP encoder reset complete! SSRC: {} → {}, {} silent frames scheduled", oldSSRC, newSSRC,
             silentFrameCount_);

        return Result<framenum_t>{frameNumber};

    } catch (const std::exception &e) {
        const auto errorMsg = fmt::format("RtpEncoderResetEvent failed: {}", e.what());
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    } catch (...) {
        const auto errorMsg = "RtpEncoderResetEvent failed with unknown error";
        error(errorMsg);
        if (span)
            span->setError(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }
}

RtpSilentFrameEvent::RtpSilentFrameEvent(framenum_t frameNumber_, uint8_t framesRemaining_)
    : EventBase(frameNumber_), framesRemaining_(framesRemaining_) {}

Result<framenum_t> RtpSilentFrameEvent::executeImpl() {
    if (framesRemaining_ == 0) {
        return Result<framenum_t>{frameNumber};
    }
    if (!rtpServer || !rtpServer->isReady()) {
        return Result<framenum_t>{
            ServerError(ServerError::InternalError, "RTP server unavailable during decoder priming")};
    }

    const uint32_t timestamp = rtpServer->getNextFrameTimestamp();
    const auto sendResult = rtpServer->sendSilentFrame();
    if (sendResult != RTP_OK) {
        return Result<framenum_t>{ServerError(ServerError::InternalError,
                                              fmt::format("RTP silent frame send failed at timestamp {} with error {}",
                                                          timestamp, static_cast<int>(sendResult)))};
    }

    if (metrics) {
        metrics->incrementRtpEventsProcessed();
    }

    if (framesRemaining_ > 1) {
        if (!eventLoop) {
            return Result<framenum_t>{
                ServerError(ServerError::InternalError, "Event loop unavailable during decoder priming")};
        }
        constexpr framenum_t primingStep = RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;
        eventLoop->scheduleEvent(
            std::make_shared<RtpSilentFrameEvent>(frameNumber + primingStep, framesRemaining_ - 1));
    }

    return Result<framenum_t>{frameNumber};
}

} // namespace creatures
