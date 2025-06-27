//
// RptServer.h
//

#pragma once

#include <uvgrtp/lib.hh>

namespace creatures :: rtp {

    class RtpServer {

    public:
        RtpServer() = default;
        ~RtpServer() = default;

        void start();
        void stop();

        /**
         * Send multi-channel audio data over RTP
         *
         * @param data Raw audio payload (header + interleaved 16-bit PCM data)
         * @param size Size of the payload in bytes
         * @return RTP_OK on success, error code on failure
         */
        rtp_error_t sendMultiChannelAudio(const uint8_t* data, size_t size);

        /**
         * Check if the RTP stream is ready for sending
         */
        bool isReady() const { return stream != nullptr; }

    private:
        uvgrtp::context         ctx;
        uvgrtp::session*        session = nullptr;
        uvgrtp::media_stream*   stream = nullptr;
    };

} // creatures :: rtp