
#pragma once

#include <atomic>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

/**
 * A helper class to keep track of some counters for system usage
 */
namespace creatures {

/*
 * This one is weird. The object is UNDER the DTO.
 */

#include OATPP_CODEGEN_BEGIN(DTO)

class SystemCountersDto : public oatpp::DTO {

    DTO_INIT(SystemCountersDto, DTO /* extends */)

    DTO_FIELD_INFO(totalFrames) { info->description = "Number of frames that have been processed"; }
    DTO_FIELD(UInt64, totalFrames);

    DTO_FIELD_INFO(eventsProcessed) {
        info->description = "Number of events that have been processed by the event loop";
    }
    DTO_FIELD(UInt64, eventsProcessed);

    DTO_FIELD_INFO(framesStreamed) {
        info->description = "Number of streaming frames that have been received from clients";
    }
    DTO_FIELD(UInt64, framesStreamed);

    DTO_FIELD_INFO(dmxEventsProcessed) { info->description = "Number of DMX events that have been processed"; }
    DTO_FIELD(UInt64, dmxEventsProcessed);

    DTO_FIELD_INFO(animationsPlayed) { info->description = "Number of animations that have been played"; }
    DTO_FIELD(UInt64, animationsPlayed);

    DTO_FIELD_INFO(soundsPlayed) { info->description = "Number of sounds that have been played"; }
    DTO_FIELD(UInt64, soundsPlayed);

    DTO_FIELD_INFO(playlistsStarted) { info->description = "Number of playlists that have been started"; }
    DTO_FIELD(UInt64, playlistsStarted);

    DTO_FIELD_INFO(playlistsStopped) { info->description = "Number of playlists that have been stopped"; }
    DTO_FIELD(UInt64, playlistsStopped);

    DTO_FIELD_INFO(playlistsEventsProcessed) {
        info->description = "Number of events that have been processed by the playlist system";
    }
    DTO_FIELD(UInt64, playlistsEventsProcessed);

    DTO_FIELD_INFO(playlistStatusRequests) { info->description = "Number of requests for playlist status"; }
    DTO_FIELD(UInt64, playlistStatusRequests);

    DTO_FIELD_INFO(restRequestsProcessed) { info->description = "Number of RESTful requests that have been processed"; }
    DTO_FIELD(UInt64, restRequestsProcessed);

    DTO_FIELD_INFO(rtpEventsProcessed) { info->description = "Number of RTP events that have been processed"; }
    DTO_FIELD(UInt64, rtpEventsProcessed);

    DTO_FIELD_INFO(rtpSendFailures) { info->description = "Number of RTP output sends that have failed"; }
    DTO_FIELD(UInt64, rtpSendFailures);

    DTO_FIELD_INFO(rtpSendFailuresSuppressed) {
        info->description = "Number of RTP send failures whose detailed log/span was rate-limited away";
    }
    DTO_FIELD(UInt64, rtpSendFailuresSuppressed);

    DTO_FIELD_INFO(rtpSendRecoveries) {
        info->description = "Number of times RTP output sends recovered after a run of failures";
    }
    DTO_FIELD(UInt64, rtpSendRecoveries);

    DTO_FIELD_INFO(rtpCircuitBreakerTrips) {
        info->description =
            "Number of times the RTP send-failure circuit breaker opened and terminated an output generation";
    }
    DTO_FIELD(UInt64, rtpCircuitBreakerTrips);

    DTO_FIELD_INFO(rtcpReportsSent) {
        info->description = "Number of RTCP Sender Report compound packets successfully sent";
    }
    DTO_FIELD(UInt64, rtcpReportsSent);

    DTO_FIELD_INFO(rtcpSendFailures) { info->description = "Number of RTCP Sender Report sends that have failed"; }
    DTO_FIELD(UInt64, rtcpSendFailures);

    DTO_FIELD_INFO(rtpAudioLoadersActive) {
        info->description = "Number of cooperative RTP audio loader jobs currently running";
    }
    DTO_FIELD(UInt64, rtpAudioLoadersActive);

    DTO_FIELD_INFO(rtpAudioLoadsQueued) {
        info->description = "Number of cooperative RTP audio loader jobs waiting for a worker";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsQueued);

    DTO_FIELD_INFO(rtpAudioLoadsAccepted) {
        info->description = "Number of cooperative RTP audio loader jobs admitted";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsAccepted);

    DTO_FIELD_INFO(rtpAudioLoadsCompleted) {
        info->description = "Number of cooperative RTP audio loader jobs completed";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsCompleted);

    DTO_FIELD_INFO(rtpAudioLoadsRejected) {
        info->description = "Number of cooperative RTP audio loader submissions rejected at admission";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsRejected);

    DTO_FIELD_INFO(rtpAudioLoadsCancelled) {
        info->description = "Number of queued cooperative RTP audio loader jobs abandoned before running";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsCancelled);

    DTO_FIELD_INFO(rtpAudioLoadsFailed) {
        info->description = "Number of cooperative RTP audio loader jobs that threw an exception";
    }
    DTO_FIELD(UInt64, rtpAudioLoadsFailed);

    DTO_FIELD_INFO(localAudioPlaybacksActive) {
        info->description = "Number of local audio device jobs currently running";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksActive);

    DTO_FIELD_INFO(localAudioPlaybacksQueued) {
        info->description = "Number of local audio device jobs waiting in the last-request-wins slot";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksQueued);

    DTO_FIELD_INFO(localAudioPlaybacksAccepted) { info->description = "Number of local audio playback jobs admitted"; }
    DTO_FIELD(UInt64, localAudioPlaybacksAccepted);

    DTO_FIELD_INFO(localAudioPlaybacksCompleted) {
        info->description = "Number of local audio playback jobs completed";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksCompleted);

    DTO_FIELD_INFO(localAudioPlaybacksReplaced) {
        info->description = "Number of local audio playback jobs replaced by a newer request";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksReplaced);

    DTO_FIELD_INFO(localAudioPlaybacksRejected) {
        info->description = "Number of local audio playback jobs rejected during admission";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksRejected);

    DTO_FIELD_INFO(localAudioPlaybacksStopped) {
        info->description = "Number of local audio playback jobs explicitly stopped or stopped at shutdown";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksStopped);

    DTO_FIELD_INFO(localAudioPlaybacksFailed) { info->description = "Number of local audio playback jobs that failed"; }
    DTO_FIELD(UInt64, localAudioPlaybacksFailed);

    DTO_FIELD_INFO(localAudioPlaybacksTimedOut) {
        info->description = "Number of local audio playback jobs that timed out";
    }
    DTO_FIELD(UInt64, localAudioPlaybacksTimedOut);

    DTO_FIELD_INFO(soundFilesServed) { info->description = "Number of sound files that have been served"; }
    DTO_FIELD(UInt64, soundFilesServed);

    DTO_FIELD_INFO(websocketConnectionsProcessed) {
        info->description = "Number of websocket connections that have been processed";
    }
    DTO_FIELD(UInt64, websocketConnectionsProcessed);

    DTO_FIELD_INFO(websocketMessagesReceived) {
        info->description = "Number of messages that have been received by the web socket";
    }
    DTO_FIELD(UInt64, websocketMessagesReceived);

    DTO_FIELD_INFO(websocketMessagesSent) {
        info->description = "Number of messages that have been sent by the web socket";
    }
    DTO_FIELD(UInt64, websocketMessagesSent);

    DTO_FIELD_INFO(websocketPingsSent) { info->description = "Number of pings that have been sent by the web socket"; }
    DTO_FIELD(UInt64, websocketPingsSent);

    DTO_FIELD_INFO(websocketPongsReceived) {
        info->description = "Number of pongs that have been received by the web socket";
    }
    DTO_FIELD(UInt64, websocketPongsReceived);

    DTO_FIELD_INFO(rtpEncoderResets) {
        info->description = "Number of RTP encoder resets (SSRC rotations) that have been performed";
    }
    DTO_FIELD(UInt64, rtpEncoderResets);
};

#include OATPP_CODEGEN_END(DTO)

class SystemCounters {

  public:
    SystemCounters();
    ~SystemCounters() = default;

    void incrementTotalFrames();
    void incrementEventsProcessed();
    void incrementFramesStreamed();
    void incrementDMXEventsProcessed();
    void incrementAnimationsPlayed();
    void incrementSoundsPlayed();
    void incrementPlaylistsStarted();
    void incrementPlaylistsStopped();
    void incrementPlaylistsEventsProcessed();
    void incrementPlaylistStatusRequests();
    void incrementRtpEventsProcessed();
    void incrementRtpSendFailures();
    void incrementRtpSendFailuresSuppressed();
    void incrementRtpSendRecoveries();
    void incrementRtpCircuitBreakerTrips();
    void incrementRtcpReportsSent();
    void incrementRtcpSendFailures();
    void incrementRtpEncoderResets();
    void incrementRestRequestsProcessed();
    void incrementSoundFilesServed();
    void incrementWebsocketConnectionsProcessed();
    void incrementWebsocketMessagesReceived();
    void incrementWebsocketMessagesSent();
    void incrementWebsocketPingsSent();
    void incrementWebsocketPongsReceived();
    void setRtpAudioLoadMetrics(uint64_t active, uint64_t queued, uint64_t accepted, uint64_t completed,
                                uint64_t rejected, uint64_t cancelled, uint64_t failed);
    void setLocalAudioPlaybackMetrics(uint64_t active, uint64_t queued, uint64_t accepted, uint64_t completed,
                                      uint64_t replaced, uint64_t rejected, uint64_t stopped, uint64_t failed,
                                      uint64_t timedOut);

    uint64_t getTotalFrames();
    uint64_t getEventsProcessed();
    uint64_t getFramesStreamed();
    uint64_t getDMXEventsProcessed();
    uint64_t getAnimationsPlayed();
    uint64_t getSoundsPlayed();
    uint64_t getPlaylistsStarted();
    uint64_t getPlaylistsStopped();
    uint64_t getPlaylistsEventsProcessed();
    uint64_t getPlaylistStatusRequests();
    uint64_t getRestRequestsProcessed();
    uint64_t getRtpEventsProcessed();
    uint64_t getRtpSendFailures();
    uint64_t getRtpSendFailuresSuppressed();
    uint64_t getRtpSendRecoveries();
    uint64_t getRtpCircuitBreakerTrips();
    uint64_t getRtcpReportsSent();
    uint64_t getRtcpSendFailures();
    uint64_t getRtpEncoderResets();
    uint64_t getRtpAudioLoadersActive();
    uint64_t getRtpAudioLoadsQueued();
    uint64_t getRtpAudioLoadsAccepted();
    uint64_t getRtpAudioLoadsCompleted();
    uint64_t getRtpAudioLoadsRejected();
    uint64_t getRtpAudioLoadsCancelled();
    uint64_t getRtpAudioLoadsFailed();
    uint64_t getLocalAudioPlaybacksActive();
    uint64_t getLocalAudioPlaybacksQueued();
    uint64_t getLocalAudioPlaybacksAccepted();
    uint64_t getLocalAudioPlaybacksCompleted();
    uint64_t getLocalAudioPlaybacksReplaced();
    uint64_t getLocalAudioPlaybacksRejected();
    uint64_t getLocalAudioPlaybacksStopped();
    uint64_t getLocalAudioPlaybacksFailed();
    uint64_t getLocalAudioPlaybacksTimedOut();
    uint64_t getSoundFilesServed();
    uint64_t getWebsocketConnectionsProcessed();
    uint64_t getWebsocketMessagesReceived();
    uint64_t getWebsocketMessagesSent();
    uint64_t getWebsocketPingsSent();
    uint64_t getWebsocketPongsReceived();

    // This one is different for how it gets to a DTO since it's not a normal type of object
    oatpp::Object<SystemCountersDto> convertToDto();

  private:
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> eventsProcessed;
    std::atomic<uint64_t> framesStreamed;
    std::atomic<uint64_t> dmxEventsProcessed;
    std::atomic<uint64_t> animationsPlayed;
    std::atomic<uint64_t> soundsPlayed;
    std::atomic<uint64_t> playlistsStarted;
    std::atomic<uint64_t> playlistsStopped;
    std::atomic<uint64_t> playlistsEventsProcessed;
    std::atomic<uint64_t> playlistStatusRequests;
    std::atomic<uint64_t> restRequestsProcessed;
    std::atomic<uint64_t> soundFilesServed;
    std::atomic<uint64_t> rtpEventsProcessed;
    std::atomic<uint64_t> rtpSendFailures;
    std::atomic<uint64_t> rtpSendFailuresSuppressed;
    std::atomic<uint64_t> rtpSendRecoveries;
    std::atomic<uint64_t> rtpCircuitBreakerTrips;
    std::atomic<uint64_t> rtcpReportsSent;
    std::atomic<uint64_t> rtcpSendFailures;
    std::atomic<uint64_t> rtpEncoderResets;
    std::atomic<uint64_t> rtpAudioLoadersActive;
    std::atomic<uint64_t> rtpAudioLoadsQueued;
    std::atomic<uint64_t> rtpAudioLoadsAccepted;
    std::atomic<uint64_t> rtpAudioLoadsCompleted;
    std::atomic<uint64_t> rtpAudioLoadsRejected;
    std::atomic<uint64_t> rtpAudioLoadsCancelled;
    std::atomic<uint64_t> rtpAudioLoadsFailed;
    std::atomic<uint64_t> localAudioPlaybacksActive;
    std::atomic<uint64_t> localAudioPlaybacksQueued;
    std::atomic<uint64_t> localAudioPlaybacksAccepted;
    std::atomic<uint64_t> localAudioPlaybacksCompleted;
    std::atomic<uint64_t> localAudioPlaybacksReplaced;
    std::atomic<uint64_t> localAudioPlaybacksRejected;
    std::atomic<uint64_t> localAudioPlaybacksStopped;
    std::atomic<uint64_t> localAudioPlaybacksFailed;
    std::atomic<uint64_t> localAudioPlaybacksTimedOut;
    std::atomic<uint64_t> websocketConnectionsProcessed;
    std::atomic<uint64_t> websocketMessagesReceived;
    std::atomic<uint64_t> websocketMessagesSent;
    std::atomic<uint64_t> websocketPingsSent;
    std::atomic<uint64_t> websocketPongsReceived;
};

} // namespace creatures
