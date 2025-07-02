// src/server/rtp/MultiOpusRtpServer.cpp
#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <uvgrtp/lib.hh>
#include <uvgrtp/util.hh>

#include "server/config.h"
#include "MultiOpusRtpServer.h"

using namespace creatures::rtp;


MultiOpusRtpServer::MultiOpusRtpServer()
{
    try {
        for (size_t i = 0; i < RTP_STREAMING_CHANNELS; ++i) {
            // 1.  create_session() with multicast address
            sess_[i] = ctx_.create_session(RTP_GROUPS[i]);

            // 2.  create_stream():     localPort, remotePort, flags, format
            streams_[i] = sess_[i]->create_stream(
                RTP_PORT,          /* src */
                RTP_PORT,          /* dst */
                RTP_FORMAT_OPUS,   /* fmt  */
                RCE_SEND_ONLY);    /* flags */

            // 3.  Override the dynamic PT so VLC/Wireshark see 96
            streams_[i]->configure_ctx(RCC_DYN_PAYLOAD_TYPE, RTP_OPUS_PAYLOAD_PT);

            // (Optional) 4.  Tell uvgrtp the real clock rate (48 kHz)
            streams_[i]->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE);
        }
        ready_ = true;
    }
    catch (const std::exception& e) {
        spdlog::error("🐇 MultiOpusRtpServer init failed: {}", e.what());
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