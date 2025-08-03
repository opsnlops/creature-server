/**
 * @file MultiOpusRtpServer.cpp
 * @brief Implementation of the multi-channel Opus RTP streaming server
 *
 * This file contains the implementation of the MultiOpusRtpServer class
 * which manages multiple RTP streams for Opus-encoded audio channels.
 */

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <uvgrtp/lib.hh>
#include <uvgrtp/media_stream.hh>
#include <uvgrtp/util.hh>

#include "MultiOpusRtpServer.h"
#include "server/config.h"
#include "server/namespace-stuffs.h"

using namespace creatures::rtp;

MultiOpusRtpServer::MultiOpusRtpServer()
    : silentFrameBuffer_(RTP_SAMPLES, 0) // Pre-allocate silent frame (all zeros)
{
    try {
        for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            // Create session with multicast address
            rtpSessions_[channelIndex] = rtpContext_.create_session(RTP_GROUPS[channelIndex]);

            // Create media stream with local port, remote port, format, and flags
            mediaStreams_[channelIndex] = rtpSessions_[channelIndex]->create_stream(RTP_PORT,        // source port
                                                                                    RTP_PORT,        // destination port
                                                                                    RTP_FORMAT_OPUS, // format
                                                                                    RCE_SEND_ONLY);  // flags

            // Override the dynamic payload type so VLC/Wireshark recognize Opus (payload type 96)
            mediaStreams_[channelIndex]->configure_ctx(RCC_DYN_PAYLOAD_TYPE, RTP_OPUS_PAYLOAD_PT);

            // Configure the clock rate for accurate timing (48 kHz)
            mediaStreams_[channelIndex]->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE);

            // Create Opus encoder for this channel
            opusEncoders_[channelIndex] = std::make_unique<opus::Encoder>();
        }

        // Set initial SSRC values - each channel gets its own sequential SSRC
        rotateSynchronizationSourceIdentifiers();
        isServerReady_ = true;

        info("MultiOpusRtpServer initialized with {} channels, starting SSRC: {}", RTP_STREAMING_CHANNELS,
             currentSynchronizationSourceIdentifier_);
    } catch (const std::exception &exception) {
        error("MultiOpusRtpServer initialization failed: {}", exception.what());
    }
}

MultiOpusRtpServer::~MultiOpusRtpServer() {
    for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        if (rtpSessions_[channelIndex] && mediaStreams_[channelIndex]) {
            rtpSessions_[channelIndex]->destroy_stream(mediaStreams_[channelIndex]);
        }
        if (rtpSessions_[channelIndex]) {
            rtpContext_.destroy_session(rtpSessions_[channelIndex]);
        }
    }
}

rtp_error_t MultiOpusRtpServer::send(uint8_t channelIndex, const std::vector<uint8_t> &opusEncodedFrame) {
    if (channelIndex >= RTP_STREAMING_CHANNELS || !mediaStreams_[channelIndex]) {
        return RTP_INVALID_VALUE;
    }

    return mediaStreams_[channelIndex]->push_frame(
        const_cast<uint8_t *>(opusEncodedFrame.data()), // uvgRTP requires non-const pointer
        opusEncodedFrame.size(), RTP_NO_FLAGS);
}

void MultiOpusRtpServer::rotateSynchronizationSourceIdentifiers() {
    currentSynchronizationSourceIdentifier_ = nextSynchronizationSourceIdentifier_;

    for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        if (mediaStreams_[channelIndex]) {
            // Assign fresh SSRC, then increment for next channel
            mediaStreams_[channelIndex]->configure_ctx(RCC_SSRC, nextSynchronizationSourceIdentifier_++);
        }
    }

    debug("SSRC rotated! New range: {} to {}", currentSynchronizationSourceIdentifier_,
          nextSynchronizationSourceIdentifier_ - 1);
}

void MultiOpusRtpServer::resetAllEncoders() {
    for (size_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
        if (opusEncoders_[channelIndex]) {
            opusEncoders_[channelIndex]->reset();
        }
    }
    debug("All Opus encoders reset to initial state");
}

void MultiOpusRtpServer::sendSilentFrames(uint8_t numberOfFrames) {
    debug("Sending {} silent frames to prime decoders", numberOfFrames);

    for (uint8_t frameIndex = 0; frameIndex < numberOfFrames; ++frameIndex) {
        for (uint8_t channelIndex = 0; channelIndex < RTP_STREAMING_CHANNELS; ++channelIndex) {
            if (opusEncoders_[channelIndex]) {
                try {
                    // Encode silent frame
                    auto encodedSilentFrame = opusEncoders_[channelIndex]->encode(silentFrameBuffer_.data());

                    // Send the encoded silent frame
                    auto transmissionResult = send(channelIndex, encodedSilentFrame);
                    if (transmissionResult != RTP_OK) {
                        warn("Failed to send silent frame {} on channel {}: error {}", frameIndex, channelIndex,
                             transmissionResult);
                    }
                } catch (const std::exception &exception) {
                    error("Error encoding/sending silent frame {} on channel {}: {}", frameIndex, channelIndex,
                          exception.what());
                }
            }
        }

        // Small delay between frames (not strictly necessary but helps with debugging)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    debug("Silent frame priming complete - decoders should be warmed up");
}