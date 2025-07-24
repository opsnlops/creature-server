// src/server/rtp/MultiOpusRtpServer.h

#pragma once

#include <array>
#include <uvgrtp/lib.hh>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"

namespace creatures::rtp {
class MultiOpusRtpServer {
  public:
    MultiOpusRtpServer();
    ~MultiOpusRtpServer();

    // Send one 10 ms *mono* frame for a given channel [0-16]
    rtp_error_t send(uint8_t chan, const std::vector<uint8_t> &opusFrame);

    // Rotate SSRC for all channels and reset encoders - fresh start! 🐰
    void rotateSSRC();

    // Reset all Opus encoders to clean state
    void resetEncoders();

    // Send silent frames to prime the decoders - like warming up before a hop! 🐰
    void sendSilentFrames(uint8_t numFrames = 4);

    [[nodiscard]] bool isReady() const { return ready_; }

    // Get current SSRC (useful for debugging)
    [[nodiscard]] uint32_t getCurrentSSRC() const { return current_ssrc_; }

  private:
    bool ready_{false};
    uint32_t next_ssrc_{1000}; // Start from a nice round number for easy debugging
    uint32_t current_ssrc_{0}; // Track current SSRC for logging

    uvgrtp::context ctx_;
    std::array<uvgrtp::session *, RTP_STREAMING_CHANNELS> sess_{};
    std::array<uvgrtp::media_stream *, RTP_STREAMING_CHANNELS> streams_{};

    // Store encoders for reset capability
    std::array<std::unique_ptr<opus::Encoder>, RTP_STREAMING_CHANNELS> encoders_;

    // Pre-allocated silent frame for priming
    std::vector<int16_t> silent_frame_;
};
} // namespace creatures::rtp