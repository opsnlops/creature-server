//
// RtpServer.h
//

#pragma once

#include <string>

#include <uvgrtp/lib.hh>

namespace creatures :: rtp {

    /**
     * Simple RTP server for multicast audio streaming
     * No RTSP overhead - just pure RTP goodness! 🐰
     */
    class RtpServer {

    public:
        RtpServer() = default;
        ~RtpServer() = default;

        /**
         * Start the RTP multicast stream
         */
        void start();

        /**
         * Stop the RTP stream
         */
        void stop();

        /**
         * Send multi-channel audio data over RTP
         *
         * @param data Raw 16-bit interleaved PCM audio payload
         * @param size Size of the payload in bytes
         * @return RTP_OK on success, error code on failure
         */
        rtp_error_t sendMultiChannelAudio(const uint8_t* data, size_t size);

        /**
         * Check if the RTP stream is ready for sending
         */
        [[nodiscard]] bool isReady() const { return rtpStream != nullptr; }

        /**
         * Get the SDP description for this audio stream
         * Clients can use this to configure their receivers
         */
        [[nodiscard]] std::string getSdpDescription() const;

        /**
         * Get the multicast URL for this stream
         * Format: rtp://multicast_ip:port
         */
        static std::string getMulticastUrl() ;

    private:
        uvgrtp::context         ctx;
        uvgrtp::session*        session = nullptr;
        uvgrtp::media_stream*   rtpStream = nullptr;

        /**
         * Generate SDP description for our 17-channel L16 audio stream
         */
        static std::string generateSdp() ;
    };

} // creatures :: rtp