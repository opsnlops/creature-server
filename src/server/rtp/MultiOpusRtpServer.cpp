// src/server/rtp/MultiOpusRtpServer.cpp
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <uvgrtp/lib.hh>
#include <uvgrtp/media_stream.hh>
#include <uvgrtp/util.hh>

#include "server/config.h"
#include "MultiOpusRtpServer.h"

using namespace creatures::rtp;

MultiOpusRtpServer::MultiOpusRtpServer()
    : silent_frame_(RTP_SAMPLES, 0)  // Pre-allocate silent frame (all zeros)
{
    try {
        for (size_t i = 0; i < RTP_STREAMING_CHANNELS; ++i) {
            // 1. Create session with multicast address
            sess_[i] = ctx_.create_session(RTP_GROUPS[i]);

            // 2. Create stream: localPort, remotePort, flags, format
            streams_[i] = sess_[i]->create_stream(
                RTP_PORT,          /* src */
                RTP_PORT,          /* dst */
                RTP_FORMAT_OPUS,   /* fmt  */
                RCE_SEND_ONLY);    /* flags */

            // 3. Override the dynamic PT so VLC/Wireshark see 96
            streams_[i]->configure_ctx(RCC_DYN_PAYLOAD_TYPE, RTP_OPUS_PAYLOAD_PT);

            // 4. Tell uvgrtp the real clock rate (48 kHz)
            streams_[i]->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE);

            // 5. Create Opus encoder for this channel
            encoders_[i] = std::make_unique<opus::Encoder>();
        }

        // Set initial SSRC values - each channel gets its own sequential SSRC
        rotateSSRC();
        ready_ = true;

        spdlog::info("MultiOpusRtpServer initialized with {} channels, starting SSRC: {}",
                     RTP_STREAMING_CHANNELS, current_ssrc_);
    }
    catch (const std::exception& e) {
        spdlog::error("MultiOpusRtpServer init failed - this bunny couldn't hop! 🐰: {}", e.what());
    }
}

MultiOpusRtpServer::~MultiOpusRtpServer()
{
    for (size_t i = 0; i < RTP_STREAMING_CHANNELS; ++i) {
        if (sess_[i] && streams_[i])
            sess_[i]->destroy_stream(streams_[i]);
        if (sess_[i])
            ctx_.destroy_session(sess_[i]);
    }
}

rtp_error_t MultiOpusRtpServer::send(uint8_t chan,
                                     const std::vector<uint8_t>& frame)
{
    if (chan >= RTP_STREAMING_CHANNELS || !streams_[chan])
        return RTP_INVALID_VALUE;

    return streams_[chan]->push_frame(
        const_cast<uint8_t *>(frame.data()), // uvgRTP needs non-const
        frame.size(),
        RTP_NO_FLAGS);
}

void MultiOpusRtpServer::rotateSSRC()
{
    current_ssrc_ = next_ssrc_;

    for (size_t i = 0; i < RTP_STREAMING_CHANNELS; ++i) {
        if (streams_[i]) {
            // Assign fresh SSRC, then increment for next channel
            streams_[i]->configure_ctx(RCC_SSRC, next_ssrc_++);
        }
    }

    spdlog::debug("🐰 SSRC rotated! New range: {} to {} - fresh hops for all channels!",
                  current_ssrc_, next_ssrc_ - 1);
}

void MultiOpusRtpServer::resetEncoders()
{
    for (size_t i = 0; i < RTP_STREAMING_CHANNELS; ++i) {
        if (encoders_[i]) {
            encoders_[i]->reset();
        }
    }
    spdlog::debug("🐰 All Opus encoders reset - ready for a fresh hop!");
}

void MultiOpusRtpServer::sendSilentFrames(uint8_t numFrames)
{
    spdlog::debug("Sending {} silent frames to prime decoders", numFrames);

    for (uint8_t frame = 0; frame < numFrames; ++frame) {
        for (uint8_t chan = 0; chan < RTP_STREAMING_CHANNELS; ++chan) {
            if (encoders_[chan]) {
                try {
                    // Encode silent frame
                    auto encodedFrame = encoders_[chan]->encode(silent_frame_.data());

                    // Send it
                    auto result = send(chan, encodedFrame);
                    if (result != RTP_OK) {
                        spdlog::warn("Failed to send silent frame {} on channel {}: error {}",
                                   frame, chan, result);
                    }
                } catch (const std::exception& e) {
                    spdlog::error("Error encoding/sending silent frame {} on channel {}: {}",
                                frame, chan, e.what());
                }
            }
        }

        // Small delay between frames (not strictly necessary but nice for debugging)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    spdlog::debug("🐰 Silent frame priming complete - decoders should be warmed up!");
}