//
// RtpServer.cpp
//

#include <uvgrtp/lib.hh>
#include <fmt/format.h>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/namespace-stuffs.h"

#include "RtpServer.h"

namespace creatures {
    extern std::shared_ptr<Configuration> config;
}

namespace creatures :: rtp {

    void RtpServer::start() {
        info("Starting RTP server for audio streaming");

        // Create session using multicast for the RTP stream
        session = ctx.create_session(RTP_MULTICAST_GROUP);
        if (!session) {
            error("Failed to create RTP session");
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

        // Create RTP stream for audio - provide destination port for SEND_ONLY mode
        rtpStream = session->create_stream(
            RTP_PORT, RTP_PORT,  // RTP port for both local and destination
            RTP_FORMAT_GENERIC,
            sendFlags
        );

        if (!rtpStream) {
            error("Failed to create RTP stream");
            session = nullptr;
            return;
        }

        // Configure the RTP stream for L16 audio (16-bit linear PCM, network byte order)
        rtpStream->configure_ctx(RCC_DYN_PAYLOAD_TYPE, 97);  // Dynamic payload type for L16
        rtpStream->configure_ctx(RCC_CLOCK_RATE, RTP_SRATE); // 48000 Hz sample rate
        rtpStream->configure_ctx(RCC_MULTICAST_TTL, 16);     // Allow multicast routing

        // Configure MTU based on fragmentation setting
        if (config->getRtpFragmentPackets()) {
            rtpStream->configure_ctx(RCC_MTU_SIZE, 1500);  // Standard Ethernet MTU
        } else {
            rtpStream->configure_ctx(RCC_MTU_SIZE, 9000);  // Jumbo frame MTU
        }

        // Note: uvgRTP handles sequence numbering automatically
        // We'll monitor for any send errors in the sendMultiChannelAudio method

        // L16 format note: While RFC 3551 specifies network byte order,
        // many implementations expect host byte order for direct audio processing
        debug("Configuring RTP for L16 format (16-bit linear PCM, host byte order for compatibility)");

        info("🎵 RTP server configured successfully!");
        info("  • Multicast RTP: {}:{}", RTP_MULTICAST_GROUP, RTP_PORT);
        info("  • Format: L16 {} channels at {}Hz (host byte order)", RTP_STREAMING_CHANNELS, RTP_SRATE);
        info("  • Chunk size: {}ms ({} samples)", RTP_FRAME_MS, RTP_SAMPLES);
        info("  • Payload type: 97 (L16 dynamic)");
        info("  • Fragmentation: {}", config->getRtpFragmentPackets() ? "enabled" : "disabled");
        info("  • Stream URL: {}", getMulticastUrl());

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

        debug("RTP server ready - time to hop into action! 🐰");
    }

    void RtpServer::stop() {
        info("Stopping RTP server - hopping away! 🐰");

        if (rtpStream && session) {
            session->destroy_stream(rtpStream);
            rtpStream = nullptr;
        }

        if (session) {
            ctx.destroy_session(session);
            session = nullptr;
        }

        info("RTP server stopped");
    }

    rtp_error_t RtpServer::sendMultiChannelAudio(const uint8_t* data, size_t size) {
        if (!rtpStream) {
            warn("Cannot send audio - RTP stream not initialized");
            return RTP_INVALID_VALUE;
        }

        if (!data || size == 0) {
            warn("Cannot send empty audio data");
            return RTP_INVALID_VALUE;
        }

        // Calculate expected size for typical chunks (allow variable size for last chunk)
        size_t expectedSize = RTP_SAMPLES * RTP_STREAMING_CHANNELS * sizeof(int16_t);
        if (size != expectedSize) {
            // Only warn if significantly different (not just the last chunk)
            if (size > expectedSize || size < (expectedSize / 2)) {
                warn("Audio chunk size unusual: got {} bytes, typical {} bytes", size, expectedSize);
                debug("  Calculated from: {} samples × {} channels × 2 bytes",
                     size / (RTP_STREAMING_CHANNELS * 2), RTP_STREAMING_CHANNELS);
            } else {
                debug("Variable chunk size (likely last chunk): {} bytes vs typical {} bytes", size, expectedSize);
            }
        }

        // Size validation based on configuration
        if (config->getRtpFragmentPackets()) {
            if (size > RTP_MAX_JUMBO_FRAME_SIZE) {
                warn("Audio chunk too large even for fragmentation: {} bytes (max: {})",
                     size, RTP_MAX_JUMBO_FRAME_SIZE);
                return RTP_INVALID_VALUE;
            }
            trace("Sending {}KB L16 audio packet (will be fragmented if needed)", size / 1024);
        } else {
            if (size > RTP_STANDARD_MTU_PAYLOAD) {
                trace("Large audio chunk ({} bytes) - ensure jumbo frames are enabled", size);
            }
            trace("Sending {}KB L16 audio packet (single frame)", size / 1024);
        }

        debug("Sending RTP packet: {} bytes ({} samples per channel)", size, size / (RTP_STREAMING_CHANNELS * 2));

        // Send the raw 16-bit interleaved PCM data in host byte order
        // This matches what the client expects for direct audio processing
        rtp_error_t result = rtpStream->push_frame(const_cast<uint8_t*>(data), size, RTP_NO_FLAGS);

        if (result != RTP_OK) {
            error("RTP send failed with error: {}", static_cast<int>(result));
        } else {
            trace("Successfully sent L16 audio via RTP - {} bytes", size);
        }

        return result;
    }

    std::string RtpServer::getMulticastUrl() {
        return fmt::format("rtp://{}:{}", RTP_MULTICAST_GROUP, RTP_PORT);
    }

    std::string RtpServer::getSdpDescription() const {
        return generateSdp();
    }

    std::string RtpServer::generateSdp() {
        // Generate RFC 4566 compliant SDP for our 17-channel L16 audio stream
        // L16 is defined in RFC 3551 as 16-bit linear PCM in network byte order
        std::string sdp = fmt::format(
            "v=0\r\n"                                                    // Version
            "o=creatures 0 0 IN IP4 {}\r\n"                            // Origin
            "s=Creatures Workshop Multi-Channel Audio\r\n"              // Session name
            "i=17-channel L16 audio stream for Creatures Workshop\r\n"  // Session description
            "c=IN IP4 {}/255\r\n"                                      // Connection (multicast)
            "t=0 0\r\n"                                                 // Time (permanent session)
            "a=tool:creatures-server\r\n"                              // Tool
            "a=type:broadcast\r\n"                                     // Session type
            "m=audio {} RTP/AVP 97\r\n"                               // Media (audio, port, protocol, payload type)
            "a=rtpmap:97 L16/{}/{}\r\n"                               // RTP map (payload type, encoding, sample rate, channels)
            "a=ptime:{}\r\n"                                          // Packet time in milliseconds
            "a=fmtp:97 channel-order=FL,FR,FC,LFE,BL,BR,FLC,FRC,BC,SL,SR,TC,TFL,TFC,TFR,TBL,TBR\r\n"  // Channel layout
            "a=sendonly\r\n",                                         // Direction
            RTP_MULTICAST_GROUP,                                       // Origin IP
            RTP_MULTICAST_GROUP,                                       // Multicast IP
            RTP_PORT,                                                  // Port
            RTP_SRATE,                                                 // Sample rate
            RTP_STREAMING_CHANNELS,                                    // Channel count
            RTP_FRAME_MS                                               // Packet time
        );

        return sdp;
    }

} // creatures :: rtp