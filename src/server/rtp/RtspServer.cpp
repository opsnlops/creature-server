//
// RtspServer.cpp
//

#include <uvgrtp/lib.hh>
#include <fmt/format.h>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/namespace-stuffs.h"

#include "RtspServer.h"

namespace creatures {
    extern std::shared_ptr<Configuration> config;
}

namespace creatures :: rtp {

    void RtspServer::start() {
        info("🐰 Starting RTSP server for multi-channel audio streaming");

        // Create session using multicast for the RTP stream
        session = ctx.create_session(RTP_MULTICAST_GROUP);
        if (!session) {
            error("Failed to create RTSP session");
            return;
        }

        // Determine send flags based on fragmentation setting
        int sendFlags = RCE_SEND_ONLY;
        if (config->getRtpFragmentPackets()) {
            sendFlags |= RCE_FRAGMENT_GENERIC;
            info("RTP packet fragmentation enabled for standard MTU networks (WiFi-friendly!)");
        } else {
            info("RTP packet fragmentation disabled (assuming jumbo frame support)");
        }

        // Create RTP stream for audio
        rtpStream = session->create_stream(
            RTP_PORT, RTP_PORT + 1,  // RTP port, RTCP port
            RTP_FORMAT_GENERIC,
            sendFlags
        );

        if (!rtpStream) {
            error("Failed to create RTP stream");
            session = nullptr;
            return;
        }

        // Configure the RTP stream for L16 audio
        rtpStream->configure_ctx(RCC_DYN_PAYLOAD_TYPE, 97);  // Dynamic payload type for L16
        rtpStream->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE); // 48000 Hz sample rate
        rtpStream->configure_ctx(RCC_MULTICAST_TTL, 16);     // Allow multicast routing

        // Configure MTU based on fragmentation setting
        if (config->getRtpFragmentPackets()) {
            rtpStream->configure_ctx(RCC_MTU_SIZE, 1500);  // Standard Ethernet MTU
        } else {
            rtpStream->configure_ctx(RCC_MTU_SIZE, 9000);  // Jumbo frame MTU
        }

        info("🎵 RTSP server configured successfully!");
        info("  • Multicast RTP: {}:{} (RTCP: {})", RTP_MULTICAST_GROUP, RTP_PORT, RTP_PORT + 1);
        info("  • Format: L16 {} channels at {}Hz", RTP_STREAMING_CHANNELS, RTP_SRATE);
        info("  • Chunk size: {}ms ({} samples)", RTP_FRAME_MS, RTP_SAMPLES);
        info("  • Payload type: 97 (L16 dynamic)");
        info("  • RTSP URL: {}", getRtspUrl());

        // Log the SDP for easy access
        info("📄 SDP Description (save as creatures-audio.sdp):");
        info("---");
        auto sdp = getSdpDescription();
        // Log each line of the SDP
        std::istringstream sdpStream(sdp);
        std::string line;
        while (std::getline(sdpStream, line)) {
            info("{}", line);
        }
        info("---");

        debug("RTSP server ready - time to hop into action! 🐰");
    }

    void RtspServer::stop() {
        info("Stopping RTSP server - hopping away! 🐰");

        if (rtpStream && session) {
            session->destroy_stream(rtpStream);
            rtpStream = nullptr;
        }

        if (session) {
            ctx.destroy_session(session);
            session = nullptr;
        }

        info("RTSP server stopped");
    }

    rtp_error_t RtspServer::sendMultiChannelAudio(const uint8_t* data, size_t size) {
        if (!rtpStream) {
            warn("Cannot send audio - RTSP/RTP stream not initialized");
            return RTP_INVALID_VALUE;
        }

        if (!data || size == 0) {
            warn("Cannot send empty audio data");
            return RTP_INVALID_VALUE;
        }

        // Size validation based on configuration
        if (config->getRtpFragmentPackets()) {
            if (size > RTP_MAX_JUMBO_FRAME_SIZE) {
                warn("Audio chunk too large even for fragmentation: {} bytes (max: {})",
                     size, RTP_MAX_JUMBO_FRAME_SIZE);
                return RTP_INVALID_VALUE;
            }
            trace("Sending {}KB L16 audio packet (will be fragmented)", size / 1024);
        } else {
            if (size > RTP_STANDARD_MTU_PAYLOAD) {
                debug("Large audio chunk ({} bytes) - ensure jumbo frames are enabled", size);
            }
            trace("Sending {}KB L16 audio packet (single frame)", size / 1024);
        }

        // Send the raw 16-bit interleaved PCM data
        // uvgRTP will add proper RTP headers with L16 payload type 97
        rtp_error_t result = rtpStream->push_frame(const_cast<uint8_t*>(data), size, RTP_NO_FLAGS);

        if (result != RTP_OK) {
            warn("Failed to send RTSP/RTP audio packet: error {}", static_cast<int>(result));
        } else {
            trace("Successfully sent L16 audio via RTSP - pure carrot-quality audio! 🥕");
        }

        return result;
    }

    std::string RtspServer::getRtspUrl() const {
        // For multicast, the URL would typically point to an RTSP server
        // Since we're doing direct multicast RTP, provide the multicast URL
        return fmt::format("rtsp://{}:{}/creatures-audio", RTP_MULTICAST_GROUP, RTP_PORT);
    }

    std::string RtspServer::getSdpDescription() const {
        return generateSdp();
    }

    std::string RtspServer::generateSdp() const {
        // Generate RFC 4566 compliant SDP for our 17-channel L16 audio stream
        std::string sdp = fmt::format(
            "v=0\r\n"                                                    // Version
            "o=creatures 0 0 IN IP4 {}\r\n"                            // Origin
            "s=Creatures Workshop Multi-Channel Audio\r\n"              // Session name
            "i=17-channel audio stream for creature control\r\n"        // Session description
            "c=IN IP4 {}/255\r\n"                                      // Connection (multicast)
            "t=0 0\r\n"                                                 // Time (permanent session)
            "a=tool:creatures-server\r\n"                              // Tool
            "m=audio {} RTP/AVP 97\r\n"                               // Media (audio, port, protocol, payload type)
            "a=rtpmap:97 L16/{}/{}\r\n"                               // RTP map (payload type, encoding, sample rate, channels)
            "a=fmtp:97 channel-order=FL,FR,FC,LFE,BL,BR,FLC,FRC,BC,SL,SR,TC,TFL,TFC,TFR,TBL,TBR\r\n"  // Channel layout
            "a=control:trackID=1\r\n"
            "a=sendonly\r\n",
            RTP_MULTICAST_GROUP,                                       // Origin IP
            RTP_MULTICAST_GROUP,                                       // Multicast IP
            RTP_PORT,                                                  // Port
            RTP_SRATE,                                                 // Sample rate
            RTP_STREAMING_CHANNELS                                     // Channel count
        );

        return sdp;
    }

} // creatures :: rtp