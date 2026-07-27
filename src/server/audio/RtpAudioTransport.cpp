//
// RtpAudioTransport.cpp
// RTP streaming audio transport implementation
//

#include "RtpAudioTransport.h"

#include <algorithm>

#include "server/animation/PlaybackSession.h"
#include "server/metrics/counters.h"
#include "spdlog/spdlog.h"

namespace creatures {

extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SystemCounters> metrics;

RtpAudioTransport::RtpAudioTransport(std::shared_ptr<rtp::MultiOpusRtpServer> server) : rtpServer_(server) {}

Result<void> RtpAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    session_ = session;
    stopped_ = true;

    if (!session_) {
        std::string errorMsg = "No playback session provided";
        error(errorMsg);
        return Result<void>{ServerError(ServerError::InvalidData, errorMsg)};
    }

    // Validate RTP server is available
    if (!rtpServer_ || !rtpServer_->isReady()) {
        std::string errorMsg = "RTP server not ready - cannot stream audio";
        error(errorMsg);
        return Result<void>{ServerError(ServerError::InternalError, errorMsg)};
    }

    // Get audio buffer from session
    auto audioBuffer = session_->getAudioBuffer();
    if (!audioBuffer) {
        std::string errorMsg = "No audio buffer in session";
        error(errorMsg);
        return Result<void>{ServerError(ServerError::InternalError, errorMsg)};
    }

    totalFrames_ = audioBuffer->getFrameCount();
    currentFrameIndex_ = 0;
    nextDispatchFrame_ = session_->getStartingFrame();
    started_ = true;
    stopped_ = false;

    debug("RtpAudioTransport started: {} frames to dispatch", totalFrames_);

    return Result<void>{};
}

void RtpAudioTransport::stop() {
    stopped_ = true;
    debug("RtpAudioTransport stopped at frame {}/{}", currentFrameIndex_, totalFrames_);
}

Result<framenum_t> RtpAudioTransport::dispatchNextChunk(framenum_t currentFrame) {
    if (!session_) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "Missing playback session")};
    }
    if (!rtpServer_ || !rtpServer_->isReady()) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "RTP server unavailable")};
    }

    // Check if we should dispatch on this frame
    if (currentFrame < nextDispatchFrame_) {
        // Not time yet
        return Result<framenum_t>{nextDispatchFrame_};
    }

    // Check if finished or stopped
    if (stopped_ || currentFrameIndex_ >= totalFrames_) {
        return Result<framenum_t>{currentFrame};
    }

    auto audioBuffer = session_->getAudioBuffer();
    if (!audioBuffer) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "Audio buffer disappeared")};
    }

    constexpr framenum_t dispatchStep = RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS;

    // Real-time audio cannot be caught up by blasting old packets. If the event
    // loop is at least one packet late, skip to the packet that belongs at the
    // current wall-clock position and advance the RTP sample clock with it.
    if (currentFrame > nextDispatchFrame_) {
        const size_t missedFrames = static_cast<size_t>((currentFrame - nextDispatchFrame_) / dispatchStep);
        if (missedFrames > 0) {
            const size_t remainingFrames = totalFrames_ - currentFrameIndex_;
            const size_t framesToSkip = std::min(missedFrames, remainingFrames);
            currentFrameIndex_ += framesToSkip;
            nextDispatchFrame_ += static_cast<framenum_t>(framesToSkip) * dispatchStep;
            rtpServer_->advanceFrameTimestamp(framesToSkip);
            warn("RTP audio skipped {} late frame(s) for session {}", framesToSkip, session_->getSessionId());
        }
    }

    if (currentFrameIndex_ >= totalFrames_) {
        return Result<framenum_t>{currentFrame};
    }

    try {
        const uint32_t timestamp = rtpServer_->getNextFrameTimestamp();
        rtp_error_t frameResult = RTP_OK;

        // All 17 packets represent the same sample interval and therefore use
        // exactly the same RTP timestamp.
        for (int ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
            const auto sendResult =
                rtpServer_->send(static_cast<uint8_t>(ch),
                                 audioBuffer->getEncodedFrame(static_cast<uint8_t>(ch), currentFrameIndex_), timestamp);
            if (sendResult != RTP_OK && frameResult == RTP_OK) {
                frameResult = sendResult;
            }
        }

        currentFrameIndex_++;
        nextDispatchFrame_ += dispatchStep;
        rtpServer_->advanceFrameTimestamp();
        if (metrics) {
            metrics->incrementRtpEventsProcessed();
        }

        if (frameResult != RTP_OK) {
            const auto errorMsg = fmt::format("RTP send failed for audio frame {} with error {}",
                                              currentFrameIndex_ - 1, static_cast<int>(frameResult));
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }
    } catch (const std::exception &ex) {
        std::string errorMsg = fmt::format("RTP audio dispatch failed: {}", ex.what());
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    } catch (...) {
        std::string errorMsg = "RTP audio dispatch failed with unknown error";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    return Result<framenum_t>{nextDispatchFrame_};
}

std::optional<framenum_t> RtpAudioTransport::getNextDispatchFrame() const {
    if (!started_ || stopped_ || currentFrameIndex_ >= totalFrames_) {
        return std::nullopt;
    }
    return nextDispatchFrame_;
}

bool RtpAudioTransport::isFinished() const { return stopped_ || (started_ && currentFrameIndex_ >= totalFrames_); }

} // namespace creatures
