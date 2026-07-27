#pragma once

#include <mutex>

#include "AudioTransport.h"
#include "server/config.h"
#include "server/rtp/AsyncAudioTraceContext.h"
#include "server/rtp/MultiOpusRtpServer.h"

namespace creatures {

/**
 * RtpAudioTransport - RTP streaming audio transport implementation
 *
 * Sends encoded Opus audio frames to MultiOpusRtpServer frame-by-frame,
 * coordinated with DMX playback timing by the PlaybackRunnerEvent.
 *
 * Unlike the legacy bulk-scheduling approach, this transport dispatches
 * audio only when the runner requests it, keeping the event queue shallow.
 */
class RtpAudioTransport : public AudioTransport {
  public:
    /**
     * Create RTP audio transport
     *
     * @param rtpServer The RTP server to send audio to
     */
    explicit RtpAudioTransport(std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer);

    ~RtpAudioTransport() override = default;

    void setOutputLease(rtp::RtpOutputLease outputLease, rtp::AsyncAudioTraceContext traceContext = {});

    Result<void> start(std::shared_ptr<PlaybackSession> session) override;

    void stop() override;

    [[nodiscard]] bool needsPerFrameDispatch() const override { return true; }

    Result<framenum_t> dispatchNextChunk(framenum_t currentFrame) override;

    [[nodiscard]] std::optional<framenum_t> getNextDispatchFrame() const override;

    [[nodiscard]] bool isFinished() const override;

  private:
    struct OutputState {
        rtp::RtpOutputLease lease;
        rtp::AsyncAudioTraceContext traceContext;
    };

    [[nodiscard]] OutputState getOutputState() const;

    std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer_;
    std::shared_ptr<PlaybackSession> session_;
    mutable std::mutex outputStateMutex_;
    rtp::RtpOutputLease outputLease_;
    rtp::AsyncAudioTraceContext traceContext_;

    // Playback state
    size_t currentFrameIndex_{0};
    size_t totalFrames_{0};
    framenum_t nextDispatchFrame_{0};
    bool started_{false};
    bool stopped_{false};
    bool finalFrameQueued_{false};
};

} // namespace creatures
