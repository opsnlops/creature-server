/**
 * @file MultiOpusRtpServer.h
 * @brief Multi-channel Opus RTP streaming server
 *
 * This file provides a server that can stream multiple channels of Opus-encoded
 * audio over RTP multicast streams with independent SSRC management.
 */

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

    // Send one 10ms mono Opus frame for a given channel [0-16]
    rtp_error_t send(uint8_t channelIndex, const std::vector<uint8_t> &opusEncodedFrame);

    // Rotate SSRC for all channels and reset encoders for a fresh start
    void rotateSynchronizationSourceIdentifiers();

    // Reset all Opus encoders to clean state
    void resetAllEncoders();

    // Send silent frames to prime the decoders
    void sendSilentFrames(uint8_t numberOfFrames = 4);

    [[nodiscard]] bool isReady() const { return isServerReady_; }

    // Get current SSRC (useful for debugging)
    [[nodiscard]] uint32_t getCurrentSynchronizationSourceIdentifier() const {
        return currentSynchronizationSourceIdentifier_;
    }

  private:
    bool isServerReady_{false};
    uint32_t nextSynchronizationSourceIdentifier_{1000}; // Start from a round number for easy debugging
    uint32_t currentSynchronizationSourceIdentifier_{0}; // Track current SSRC for logging

    uvgrtp::context rtpContext_;
    std::array<uvgrtp::session *, RTP_STREAMING_CHANNELS> rtpSessions_{};
    std::array<uvgrtp::media_stream *, RTP_STREAMING_CHANNELS> mediaStreams_{};

    // Store encoders for reset capability
    std::array<std::unique_ptr<opus::Encoder>, RTP_STREAMING_CHANNELS> opusEncoders_;

    // Pre-allocated silent frame for priming decoders
    std::vector<int16_t> silentFrameBuffer_;
};
} // namespace creatures::rtp