//
// RtpServer.cpp
//

#include <uvgrtp/lib.hh>

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/namespace-stuffs.h"

#include "RtpServer.h"

namespace creatures {
    extern std::shared_ptr<Configuration> config;
}

namespace creatures :: rtp {

    void RtpServer::start() {
        info("starting the RTP server for multi-channel audio streaming");

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

        stream = session->create_stream(
            RTP_PORT, RTP_PORT,
            RTP_FORMAT_GENERIC,  // dynamic multi-channel PCM
            sendFlags  // Use configured fragmentation setting
        );

        if (!stream) {
            error("Failed to create RTP stream");
            session = nullptr;
            return;
        }

        // Configure multicast TTL to ensure packets can hop across network segments
        stream->configure_ctx(RCC_MULTICAST_TTL, 16);

        // Log fragmentation status for debugging
        if (config->getRtpFragmentPackets()) {
            info("RTP stream created with fragmentation support for {}KB chunks on standard MTU networks",
                 RTP_PCM_BYTES / 1024);
        } else {
            info("RTP stream created for jumbo frames ({}KB chunks)", RTP_PCM_BYTES / 1024);
        }

        info("RTP stream created successfully for {}:{}", RTP_MULTICAST_GROUP, RTP_PORT);
        debug("Configured for {} channels at {}Hz, {}ms chunks",
              RTP_STREAMING_CHANNELS, RTP_SRATE, RTP_FRAME_MS);
    }

    void RtpServer::stop() {
        info("stopping the RTP server - time to hop away! 🐰");

        if (stream && session) {
            session->destroy_stream(stream);
            stream = nullptr;
        }

        if (session) {
            ctx.destroy_session(session);
            session = nullptr;
        }

        info("RTP server stopped");
    }

    rtp_error_t RtpServer::sendMultiChannelAudio(const uint8_t* data, size_t size) {
        if (!stream) {
            warn("Cannot send audio - RTP stream not initialized");
            return RTP_INVALID_VALUE;
        }

        if (!data || size == 0) {
            warn("Cannot send empty audio data");
            return RTP_INVALID_VALUE;
        }

        // Check size limits based on fragmentation setting
        if (config->getRtpFragmentPackets()) {
            // With fragmentation, we can handle larger chunks
            if (size > RTP_MAX_JUMBO_FRAME_SIZE) {
                warn("Audio chunk too large even for fragmentation: {} bytes (max: {})",
                     size, RTP_MAX_JUMBO_FRAME_SIZE);
                return RTP_INVALID_VALUE;
            }
            trace("Sending {}KB multi-channel audio packet (will be fragmented)", size / 1024);
        } else {
            // Without fragmentation, warn if chunk is larger than standard MTU
            if (size > RTP_STANDARD_MTU_PAYLOAD) {
                debug("Large audio chunk ({} bytes) being sent without fragmentation - ensure jumbo frames are enabled",
                      size);
            }
            trace("Sending {}KB multi-channel audio packet (single frame)", size / 1024);
        }

        // Send the raw payload directly - uvgrtp will handle RTP headers and fragmentation if enabled
        rtp_error_t result = stream->push_frame(const_cast<uint8_t*>(data), size, RTP_NO_FLAGS);

        if (result != RTP_OK) {
            warn("Failed to send RTP audio packet: error {}", static_cast<int>(result));
        } else {
            trace("Successfully sent multi-channel audio packet");
        }

        return result;
    }

} // creatures :: rtp