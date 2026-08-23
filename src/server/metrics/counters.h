#pragma once

#include <atomic>
#include <cstdint>

#include <nlohmann/json.hpp>

namespace creatures {

struct SystemCountersSnapshot {
    uint64_t totalFrames = 0;
    uint64_t eventsProcessed = 0;
    uint64_t framesStreamed = 0;
    uint64_t dmxEventsProcessed = 0;
    uint64_t animationsPlayed = 0;
    uint64_t soundsPlayed = 0;
    uint64_t playlistsStarted = 0;
    uint64_t playlistsStopped = 0;
    uint64_t playlistsEventsProcessed = 0;
    uint64_t playlistStatusRequests = 0;
    uint64_t restRequestsProcessed = 0;
    uint64_t rtpEventsProcessed = 0;
    uint64_t rtpSendFailures = 0;
    uint64_t rtpSendFailuresSuppressed = 0;
    uint64_t rtpSendRecoveries = 0;
    uint64_t rtpCircuitBreakerTrips = 0;
    uint64_t rtcpReportsSent = 0;
    uint64_t rtcpSendFailures = 0;
    uint64_t rtpAudioLoadersActive = 0;
    uint64_t rtpAudioLoadsQueued = 0;
    uint64_t rtpAudioLoadsAccepted = 0;
    uint64_t rtpAudioLoadsCompleted = 0;
    uint64_t rtpAudioLoadsRejected = 0;
    uint64_t rtpAudioLoadsCancelled = 0;
    uint64_t rtpAudioLoadsFailed = 0;
    uint64_t localAudioPlaybacksActive = 0;
    uint64_t localAudioPlaybacksQueued = 0;
    uint64_t localAudioPlaybacksAccepted = 0;
    uint64_t localAudioPlaybacksCompleted = 0;
    uint64_t localAudioPlaybacksReplaced = 0;
    uint64_t localAudioPlaybacksRejected = 0;
    uint64_t localAudioPlaybacksStopped = 0;
    uint64_t localAudioPlaybacksFailed = 0;
    uint64_t localAudioPlaybacksTimedOut = 0;
    uint64_t soundFilesServed = 0;
    uint64_t websocketConnectionsProcessed = 0;
    uint64_t websocketMessagesReceived = 0;
    uint64_t websocketMessagesSent = 0;
    uint64_t websocketPingsSent = 0;
    uint64_t websocketPongsReceived = 0;
    uint64_t rtpEncoderResets = 0;
};

nlohmann::json systemCountersSnapshotToJson(const SystemCountersSnapshot &snapshot);

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

    SystemCountersSnapshot snapshot() const;

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
