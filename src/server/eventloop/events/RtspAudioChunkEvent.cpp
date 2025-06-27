//
// RtspAudioChunkEvent.cpp
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
#include "server/rtp/RtspServer.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<rtp::RtspServer> rtspServer;

    /**
     * This event type sends a pre-prepared audio chunk over RTSP/RTP
     * The audio data is already loaded and formatted when the event is created
     */
    RtspAudioChunkEvent::RtspAudioChunkEvent(framenum_t frameNumber)
            : EventBase(frameNumber) {}

    void RtspAudioChunkEvent::executeImpl() {

        // Only process if RTP mode is enabled
        if (config->getAudioMode() != Configuration::AudioMode::RTP) {
            trace("RTSP/RTP streaming is disabled, skipping RtspAudioChunkEvent");
            return;
        }

        // Ensure we have a valid RTSP server
        if (!rtspServer || !rtspServer->isReady()) {
            warn("RTSP server not available, cannot send audio chunk");
            return;
        }

        // Check we have audio data to send
        if (audioPayload.empty()) {
            warn("No audio payload available to send in RtspAudioChunkEvent");
            return;
        }

        trace("Sending {}KB L16 audio chunk via RTSP/RTP", audioPayload.size() / 1024);

        // Send the pre-prepared L16 audio payload via RTSP/RTP
        rtp_error_t result = rtspServer->sendMultiChannelAudio(audioPayload.data(), audioPayload.size());

        if (result != RTP_OK) {
            warn("Failed to send RTSP/RTP audio packet (error: {})", static_cast<int>(result));
        } else {
            trace("Successfully sent {}KB L16 audio packet via RTSP - hop-tastic! 🐰", audioPayload.size() / 1024);
        }

        // Update metrics
        metrics->incrementRtpEventsProcessed();
    }


}