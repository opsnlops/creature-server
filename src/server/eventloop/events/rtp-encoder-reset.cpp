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

RtpEncoderResetEvent::RtpEncoderResetEvent(framenum_t frameNumber_, rtp::RtpOutputLease outputLease_,
                                           rtp::AsyncAudioTraceContext traceContext_)
    : EventBase(frameNumber_), outputLease_(std::move(outputLease_)), traceContext_(std::move(traceContext_)) {}

Result<framenum_t> RtpEncoderResetEvent::executeImpl() {
    std::shared_ptr<OperationSpan> span;
    if (observability) {
        span = observability->createOperationSpan("rtp_encoder_reset_event.execute");
        span->setAttribute("frame_number", static_cast<int64_t>(frameNumber));
        span->setAttribute("silent_frame_count", static_cast<int64_t>(silentFrameCount_));
        span->setAttribute("rtp.generation", static_cast<int64_t>(outputLease_.generation));
        span->setAttribute("rtp.owner_id", outputLease_.ownerId);
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

        traceContext_.enqueueFrame = frameNumber;
        const auto enqueueResult = rtpServer->enqueueReset(outputLease_, traceContext_);
        if (enqueueResult == rtp::RtpEnqueueResult::StaleLease) {
            debug("Skipping stale RTP reset for {} generation {}", outputLease_.ownerId, outputLease_.generation);
            if (span) {
                span->setAttribute("rtp.work.outcome", "stale");
                span->setSuccess();
            }
            return Result<framenum_t>{frameNumber};
        }
        if (enqueueResult != rtp::RtpEnqueueResult::Accepted) {
            const auto errorMsg =
                fmt::format("RTP reset enqueue failed with result {}", static_cast<int>(enqueueResult));
            rtpServer->releaseOutput(outputLease_);
            if (span) {
                span->setError(errorMsg);
            }
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }

        // Prime decoders without blocking this 1ms event-loop tick.
        // Each RtpSilentFrameEvent sends one 10ms sample interval and schedules
        // the next interval independently.
        if (silentFrameCount_ > 0) {
            if (!eventLoop) {
                const auto errorMsg = "Event loop unavailable for RTP decoder priming";
                rtpServer->releaseOutput(outputLease_);
                if (span)
                    span->setError(errorMsg);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }
            eventLoop->scheduleEvent(
                std::make_shared<RtpSilentFrameEvent>(frameNumber, outputLease_, silentFrameCount_, traceContext_));
            debug("Scheduled {} silent priming frames", silentFrameCount_);
        }

        // Update metrics
        if (metrics) {
            metrics->incrementRtpEncoderResets();
        }
        if (span) {
            span->setSuccess();
        }

        info("RTP reset queued for {} generation {}; {} silent frames scheduled", outputLease_.ownerId,
             outputLease_.generation, silentFrameCount_);

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

RtpSilentFrameEvent::RtpSilentFrameEvent(framenum_t frameNumber_, rtp::RtpOutputLease outputLease_,
                                         uint8_t framesRemaining_, rtp::AsyncAudioTraceContext traceContext_)
    : EventBase(frameNumber_), framesRemaining_(framesRemaining_), outputLease_(std::move(outputLease_)),
      traceContext_(std::move(traceContext_)) {
    primingFrameIndex_ = RTP_PRIMING_FRAMES - framesRemaining_;
}

Result<framenum_t> RtpSilentFrameEvent::executeImpl() {
    if (framesRemaining_ == 0) {
        return Result<framenum_t>{frameNumber};
    }
    if (!rtpServer || !rtpServer->isReady()) {
        return Result<framenum_t>{
            ServerError(ServerError::InternalError, "RTP server unavailable during decoder priming")};
    }

    traceContext_.enqueueFrame = frameNumber;
    const auto enqueueResult = rtpServer->enqueueSilentFrame(outputLease_, primingFrameIndex_, traceContext_);
    if (enqueueResult == rtp::RtpEnqueueResult::StaleLease) {
        debug("Stopping stale RTP priming for {} generation {}", outputLease_.ownerId, outputLease_.generation);
        return Result<framenum_t>{frameNumber};
    }
    if (enqueueResult != rtp::RtpEnqueueResult::Accepted) {
        rtpServer->releaseOutput(outputLease_);
        return Result<framenum_t>{
            ServerError(ServerError::InternalError, fmt::format("RTP silent frame enqueue failed with result {}",
                                                                static_cast<int>(enqueueResult)))};
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
        eventLoop->scheduleEvent(std::make_shared<RtpSilentFrameEvent>(frameNumber + primingStep, outputLease_,
                                                                       framesRemaining_ - 1, traceContext_));
    }

    return Result<framenum_t>{frameNumber};
}

} // namespace creatures
