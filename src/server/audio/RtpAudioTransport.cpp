//
// RtpAudioTransport.cpp
// RTP streaming audio transport implementation
//

#include "RtpAudioTransport.h"

#include <algorithm>

#include "server/animation/PlaybackSession.h"
#include "server/metrics/counters.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;
extern std::shared_ptr<SystemCounters> metrics;

RtpAudioTransport::RtpAudioTransport(std::shared_ptr<rtp::MultiOpusRtpServer> server) : rtpServer_(server) {}

void RtpAudioTransport::setOutputLease(rtp::RtpOutputLease outputLease, rtp::AsyncAudioTraceContext traceContext) {
    std::lock_guard lock(outputStateMutex_);
    outputLease_ = std::move(outputLease);
    traceContext_ = std::move(traceContext);
}

RtpAudioTransport::OutputState RtpAudioTransport::getOutputState() const {
    std::lock_guard lock(outputStateMutex_);
    return OutputState{outputLease_, traceContext_};
}

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

    // Last-owner-wins is deliberate. A newer animation can supersede this
    // session after its audio has loaded but before its first runner executes.
    const auto outputState = getOutputState();
    if (!rtpServer_->isCurrentOutput(outputState.lease)) {
        started_ = true;
        warn("RTP output lease was superseded before session {} started", session_->getSessionId());
        if (auto span = session_->getSpan()) {
            span->setAttribute("rtp.generation", static_cast<int64_t>(outputState.lease.generation));
            span->setAttribute("rtp.owner_id", outputState.lease.ownerId);
            span->setAttribute("rtp.work.outcome", "stale");
        }
        return Result<void>{};
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
    if (auto span = session_->getSpan()) {
        span->setAttribute("rtp.generation", static_cast<int64_t>(outputState.lease.generation));
        span->setAttribute("rtp.owner_id", outputState.lease.ownerId);
    }

    debug("RtpAudioTransport started: {} frames to dispatch", totalFrames_);

    return Result<void>{};
}

void RtpAudioTransport::stop() {
    stopped_ = true;
    const auto outputState = getOutputState();
    if (rtpServer_ && outputState.lease.valid() && !finalFrameQueued_) {
        rtpServer_->releaseOutput(outputState.lease);
    }
    debug("RtpAudioTransport stopped at frame {}/{}", currentFrameIndex_, totalFrames_);
}

Result<framenum_t> RtpAudioTransport::dispatchNextChunk(framenum_t currentFrame) {
    if (!session_) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "Missing playback session")};
    }
    if (!rtpServer_ || !rtpServer_->isReady()) {
        return Result<framenum_t>{ServerError(ServerError::InternalError, "RTP server unavailable")};
    }
    auto outputState = getOutputState();
    if (!rtpServer_->isCurrentOutput(outputState.lease)) {
        stopped_ = true;
        debug("RTP session {} stopped because output generation {} was superseded", session_->getSessionId(),
              outputState.lease.generation);
        if (auto span = session_->getSpan()) {
            span->setAttribute("rtp.work.outcome", "stale");
        }
        return Result<framenum_t>{currentFrame};
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
    size_t framesToSkip = 0;

    // Real-time audio cannot be caught up by blasting old packets. If the event
    // loop is at least one packet late, skip to the packet that belongs at the
    // current wall-clock position and advance the RTP sample clock with it.
    if (currentFrame > nextDispatchFrame_) {
        const size_t missedFrames = static_cast<size_t>((currentFrame - nextDispatchFrame_) / dispatchStep);
        if (missedFrames > 0) {
            const size_t remainingFrames = totalFrames_ - currentFrameIndex_;
            framesToSkip = std::min(missedFrames, remainingFrames);
            currentFrameIndex_ += framesToSkip;
            nextDispatchFrame_ += static_cast<framenum_t>(framesToSkip) * dispatchStep;
            warn("RTP audio skipped {} late frame(s) for session {}", framesToSkip, session_->getSessionId());
        }
    }

    if (currentFrameIndex_ >= totalFrames_) {
        return Result<framenum_t>{currentFrame};
    }

    try {
        const bool isFinalFrame = currentFrameIndex_ + 1 >= totalFrames_;
        outputState.traceContext.enqueueFrame = currentFrame;
        const auto enqueueResult =
            rtpServer_->enqueueAudioFrame(outputState.lease, audioBuffer, currentFrameIndex_, framesToSkip,
                                          isFinalFrame, nullptr, outputState.traceContext);
        if (enqueueResult == rtp::RtpEnqueueResult::StaleLease) {
            stopped_ = true;
            return Result<framenum_t>{currentFrame};
        }
        if (enqueueResult != rtp::RtpEnqueueResult::Accepted) {
            stopped_ = true;
            rtpServer_->releaseOutput(outputState.lease);
            const auto errorMsg =
                fmt::format("RTP output queue rejected audio frame {} for session {} (result {})", currentFrameIndex_,
                            session_->getSessionId(), static_cast<int>(enqueueResult));
            return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
        }

        currentFrameIndex_++;
        finalFrameQueued_ = isFinalFrame;
        nextDispatchFrame_ += dispatchStep;
        if (metrics) {
            metrics->incrementRtpEventsProcessed();
        }
    } catch (const std::exception &ex) {
        stopped_ = true;
        rtpServer_->releaseOutput(outputState.lease);
        std::string errorMsg = fmt::format("RTP audio dispatch failed: {}", ex.what());
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    } catch (...) {
        stopped_ = true;
        rtpServer_->releaseOutput(outputState.lease);
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
