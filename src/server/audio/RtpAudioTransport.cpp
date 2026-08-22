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

    const auto outputState = getOutputState();

    // The circuit breaker releases a tripped generation's lease, so this must
    // be checked before the benign supersede case below (issue #97).
    if (rtpServer_->isGenerationTripped(outputState.lease.generation)) {
        started_ = true;
        const auto failureResult = markTerminalFailure(outputState);
        return Result<void>{failureResult.getError().value()};
    }

    // Last-owner-wins is deliberate. A newer animation can supersede this
    // session after its audio has loaded but before its first runner executes.
    if (!rtpServer_->isCurrentOutput(outputState.lease)) {
        started_ = true;
        // Re-check the registry after observing the release: the worker
        // publishes a trip's terminal record before releasing, so this closes
        // the trip-lands-between-the-two-loads race (issue #97 review).
        if (rtpServer_->isGenerationTripped(outputState.lease.generation)) {
            const auto failureResult = markTerminalFailure(outputState);
            return Result<void>{failureResult.getError().value()};
        }
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

    // Breaker-terminated generations look identical to a benign supersede once
    // the lease is released, so check the terminal registry first (issue #97).
    if (rtpServer_->isGenerationTripped(outputState.lease.generation)) {
        return markTerminalFailure(outputState);
    }

    if (!rtpServer_->isCurrentOutput(outputState.lease)) {
        // Re-check the registry after observing the release: the worker
        // publishes a trip's terminal record before releasing, so this closes
        // the trip-lands-between-the-two-loads race (issue #97 review).
        if (rtpServer_->isGenerationTripped(outputState.lease.generation)) {
            return markTerminalFailure(outputState);
        }
        stopped_ = true;
        if (finalFrameQueued_) {
            // Natural completion: the worker released the lease after sending
            // the final frame set (releaseAfterSend acknowledgment).
            return Result<framenum_t>{currentFrame};
        }
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
        // Waiting on the worker's final-frame acknowledgment. A worker that
        // hangs (rather than fails) would otherwise leave the runner ticking
        // forever, so bound the drain wait (issue #97 review).
        if (!stopped_ && finalFrameQueued_) {
            ++finalDrainTicks_;
            if (finalDrainTicks_ > MAX_FINAL_DRAIN_TICKS) {
                stopped_ = true;
                terminalFailure_ = true;
                rtpServer_->releaseOutput(outputState.lease);
                const auto errorMsg =
                    fmt::format("RTP output worker did not acknowledge the final frame set within {} ticks for "
                                "session {}; treating playback as failed",
                                MAX_FINAL_DRAIN_TICKS, session_->getSessionId());
                error(errorMsg);
                if (auto span = session_->getSpan()) {
                    span->setAttribute("rtp.work.outcome", "drain_timeout");
                    span->setError(errorMsg);
                }
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }
        }
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

bool RtpAudioTransport::isFinished() const {
    if (stopped_) {
        return true;
    }
    if (!started_ || currentFrameIndex_ < totalFrames_) {
        return false;
    }
    // Every frame is enqueued, but "enqueued" is not "delivered": when the
    // last frame set carried releaseAfterSend, the drain handshake in
    // dispatchNextChunk flips stopped_ once the worker acknowledges (releases
    // the lease), trips, or times out — so the runner keeps ticking until one
    // of those happens (issue #97).
    return !finalFrameQueued_;
}

Result<framenum_t> RtpAudioTransport::markTerminalFailure(const OutputState &outputState) {
    stopped_ = true;
    terminalFailure_ = true;

    const std::string errorMsg = fmt::format(
        "RTP playback failed for session {}: {}", session_ ? session_->getSessionId() : "unknown",
        rtpServer_ ? rtpServer_->terminalFailureMessage(outputState.lease.generation) : "RTP server unavailable");
    error(errorMsg);

    if (session_) {
        if (auto span = session_->getSpan()) {
            span->setAttribute("rtp.work.outcome", "circuit_open");
            span->setAttribute("rtp.generation", static_cast<int64_t>(outputState.lease.generation));
            span->setError(errorMsg);
        }
    }
    return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
}

} // namespace creatures
