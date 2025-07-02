// src/server/rtp/MultiOpusRtpServer.h

#pragma once

#include <array>
#include <uvgrtp/lib.hh>

#include "server/config.h"
#include "server/rtp/opus/OpusEncoderWrapper.h"

namespace creatures::rtp {

    class MultiOpusRtpServer {
    public:
        MultiOpusRtpServer();                 // creates 17 streams
        ~MultiOpusRtpServer();

        // Send one 10 ms *mono* frame for a given channel [0-16]
        rtp_error_t send(uint8_t chan, const std::vector<uint8_t>& opusFrame);

        [[nodiscard]] bool isReady() const { return ready_; }

    private:
        bool ready_{false};
        uvgrtp::context                  ctx_;
        std::array<uvgrtp::session*, RTP_STREAMING_CHANNELS> sess_{};
        std::array<uvgrtp::media_stream*, RTP_STREAMING_CHANNELS> streams_{};
    };

} // namespace creatures::rtp