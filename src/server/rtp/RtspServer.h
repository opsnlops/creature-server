//
// RtspServer.h
//

#pragma once

#include <uvgrtp/lib.hh>
#include <string>

namespace creatures :: rtp {

    class RtspServer {

    public:
        RtspServer() = default;
        ~RtspServer() = default;

        void start();
        void stop();

        /**
         * Send multi-channel audio data over RTP (via RTSP session)
         *
         * @param data Raw 16-bit interleaved PCM audio payload
         * @param size Size of the payload in bytes
         * @return RTP_OK on success, error code on failure
         */
        rtp_error_t sendMultiChannelAudio(const uint8_t* data, size_t size);

        /**
         * Check if the RTSP/RTP stream is ready for sending
         */
        bool isReady() const { return rtpStream != nullptr; }

        /**
         * Get the SDP description for this audio stream
         * This can be saved to a .sdp file or served via HTTP
         */
        std::string getSdpDescription() const;

        /**
         * Get the RTSP URL for this stream
         */
        std::string getRtspUrl() const;

    private:
        uvgrtp::context         ctx;
        uvgrtp::session*        session = nullptr;
        uvgrtp::media_stream*   rtpStream = nullptr;

        /**
         * Generate SDP description for our 17-channel L16 audio stream
         */
        std::string generateSdp() const;
    };

} // creatures :: rtp