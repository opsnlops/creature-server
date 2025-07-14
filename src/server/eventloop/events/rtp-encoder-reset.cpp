// src/server/eventloop/events/rtp-encoder-reset.cpp

#include <spdlog/spdlog.h>

#include "server/eventloop/events/types.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObservabilityManager> observability;

    RtpEncoderResetEvent::RtpEncoderResetEvent(framenum_t frameNumber, uint8_t silentFrameCount)
        : EventBase(frameNumber), silentFrameCount_(silentFrameCount) {}

    Result<framenum_t> RtpEncoderResetEvent::executeImpl()
    {
        std::shared_ptr<OperationSpan> span;
        if (observability) {
            span = observability->createOperationSpan("rtp_encoder_reset_event.execute");
            span->setAttribute("frame_number", static_cast<int64_t>(frameNumber));
            span->setAttribute("silent_frame_count", static_cast<int64_t>(silentFrameCount_));
        }

        debug("🐰 RtpEncoderResetEvent hopping into action on frame {}!", frameNumber);

        try {
            if (!rtpServer || !rtpServer->isReady()) {
                const auto errorMsg = "RTP server not available for encoder reset - bunny can't hop! 🐰";
                error(errorMsg);
                if (span) span->setError(errorMsg);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }

            // Step 1: Rotate SSRC for all channels
            const auto oldSSRC = rtpServer->getCurrentSSRC();
            rtpServer->rotateSSRC();
            const auto newSSRC = rtpServer->getCurrentSSRC();

            debug("🐰 SSRC rotated from {} to {} - fresh identity for all channels!", oldSSRC, newSSRC);

            // Step 2: Reset all Opus encoders
            rtpServer->resetEncoders();
            debug("🐰 All encoders reset - clean slate for encoding!");

            // Step 3: Send silent frames to prime decoders
            if (silentFrameCount_ > 0) {
                rtpServer->sendSilentFrames(silentFrameCount_);
                debug("Sent {} silent frames - decoders should be primed!", silentFrameCount_);
            }

            // Update metrics
            metrics->incrementRtpEncoderResets();

            if (span) {
                span->setAttribute("old_ssrc", static_cast<int64_t>(oldSSRC));
                span->setAttribute("new_ssrc", static_cast<int64_t>(newSSRC));
                span->setSuccess();
            }

            info("RTP encoder reset complete! SSRC: {} → {}, {} silent frames sent",
                 oldSSRC, newSSRC, silentFrameCount_);

            return Result<framenum_t>{frameNumber};

        } catch (const std::exception& e) {
            const auto errorMsg = fmt::format("RtpEncoderResetEvent failed: {}", e.what());
            error(errorMsg);
            if (span) span->setError(errorMsg);
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        } catch (...) {
            const auto errorMsg = "RtpEncoderResetEvent failed with unknown error";
            error(errorMsg);
            if (span) span->setError(errorMsg);
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }
    }

} // namespace creatures