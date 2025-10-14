//
// RtpAudioTransport.cpp
// RTP streaming audio transport implementation
//

#include "RtpAudioTransport.h"

#include "server/animation/PlaybackSession.h"
#include "spdlog/spdlog.h"

namespace creatures {

extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

RtpAudioTransport::RtpAudioTransport(std::shared_ptr<rtp::MultiOpusRtpServer> server) : rtpServer_(server) {}

Result<void> RtpAudioTransport::start(std::shared_ptr<PlaybackSession> session) {
    session_ = session;

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

    // Send this frame to all 17 RTP channels (16 creatures + 1 BGM)
    for (int ch = 0; ch < RTP_STREAMING_CHANNELS; ++ch) {
        rtpServer_->send(static_cast<uint8_t>(ch),
                         audioBuffer->getEncodedFrame(static_cast<uint8_t>(ch), currentFrameIndex_));
    }

    // Advance to next frame
    currentFrameIndex_++;

    // Calculate next dispatch time
    // First few frames are prefill (1ms apart), then normal pacing (20ms apart)
    if (currentFrameIndex_ < kPrefillFrames) {
        nextDispatchFrame_ = currentFrame + 1; // 1ms later
    } else {
        nextDispatchFrame_ = currentFrame + (RTP_FRAME_MS / EVENT_LOOP_PERIOD_MS); // 20ms later
    }

    return Result<framenum_t>{nextDispatchFrame_};
}

bool RtpAudioTransport::isFinished() const { return stopped_ || (started_ && currentFrameIndex_ >= totalFrames_); }

} // namespace creatures
