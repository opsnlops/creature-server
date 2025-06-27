//
// RtpAudioChunkEvent.cpp
//

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "spdlog/spdlog.h"
#include "uvgrtp/util.hh"

#include "server/config/Configuration.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/rtp/RtpServer.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<rtp::RtpServer> rtpServer;

    /**
     * This event type sends a pre-prepared audio chunk over RTP
     * The audio data is already loaded and formatted when the event is created
     */
    RtpAudioChunkEvent::RtpAudioChunkEvent(framenum_t frameNumber)
            : EventBase(frameNumber) {}

    void RtpAudioChunkEvent::executeImpl() {

        // Only process if RTP mode is enabled
        if (config->getAudioMode() != Configuration::AudioMode::RTP) {
            trace("RTP streaming is disabled, skipping RtpAudioChunkEvent");
            return;
        }

        // Ensure we have a valid RTP server
        if (!rtpServer || !rtpServer->isReady()) {
            warn("RTP server not available, cannot send audio chunk");
            return;
        }

        // Check we have audio data to send
        if (audioPayload.empty()) {
            warn("No audio payload available to send in RtpAudioChunkEvent");
            return;
        }

        trace("Sending {}KB multi-channel audio chunk", audioPayload.size() / 1024);

        // Send the pre-prepared audio payload
        rtp_error_t result = rtpServer->sendMultiChannelAudio(audioPayload.data(), audioPayload.size());

        if (result != RTP_OK) {
            warn("Failed to send multi-channel RTP audio packet (error: {})", static_cast<int>(result));
        } else {
            trace("Successfully sent {}KB multi-channel audio packet", audioPayload.size() / 1024);
        }

        // Update metrics - we should add this to the SystemCounters if it doesn't exist
        metrics->incrementRtpEventsProcessed();
    }


}