
#include "counters.h"

#include <nlohmann/json.hpp>

#include "server/ws/dto/SystemCountersDto.h"

namespace creatures {

SystemCounters::SystemCounters() {
    totalFrames = 0;
    eventsProcessed = 0;
    framesStreamed = 0;
    dmxEventsProcessed = 0;
    animationsPlayed = 0;
    soundsPlayed = 0;
    playlistsStarted = 0;
    playlistsStopped = 0;
    playlistsEventsProcessed = 0;
    playlistStatusRequests = 0;
    restRequestsProcessed = 0;
    rtpEventsProcessed = 0;
    rtpSendFailures = 0;
    rtpSendFailuresSuppressed = 0;
    rtpSendRecoveries = 0;
    rtpCircuitBreakerTrips = 0;
    rtcpReportsSent = 0;
    rtcpSendFailures = 0;
    rtpEncoderResets = 0;
    rtpAudioLoadersActive = 0;
    rtpAudioLoadsQueued = 0;
    rtpAudioLoadsAccepted = 0;
    rtpAudioLoadsCompleted = 0;
    rtpAudioLoadsRejected = 0;
    rtpAudioLoadsCancelled = 0;
    rtpAudioLoadsFailed = 0;
    localAudioPlaybacksActive = 0;
    localAudioPlaybacksQueued = 0;
    localAudioPlaybacksAccepted = 0;
    localAudioPlaybacksCompleted = 0;
    localAudioPlaybacksReplaced = 0;
    localAudioPlaybacksRejected = 0;
    localAudioPlaybacksStopped = 0;
    localAudioPlaybacksFailed = 0;
    localAudioPlaybacksTimedOut = 0;
    soundFilesServed = 0;
    websocketConnectionsProcessed = 0;
    websocketMessagesReceived = 0;
    websocketMessagesSent = 0;
    websocketPingsSent = 0;
    websocketPongsReceived = 0;
}

void SystemCounters::incrementTotalFrames() { totalFrames++; }

void SystemCounters::incrementEventsProcessed() { eventsProcessed++; }

void SystemCounters::incrementFramesStreamed() { framesStreamed++; }

void SystemCounters::incrementDMXEventsProcessed() { dmxEventsProcessed++; }

void SystemCounters::incrementAnimationsPlayed() { animationsPlayed++; }

void SystemCounters::incrementSoundsPlayed() { soundsPlayed++; }

void SystemCounters::incrementPlaylistsStarted() { playlistsStarted++; }

void SystemCounters::incrementPlaylistsStopped() { playlistsStopped++; }

void SystemCounters::incrementPlaylistsEventsProcessed() { playlistsEventsProcessed++; }

void SystemCounters::incrementPlaylistStatusRequests() { playlistStatusRequests++; }

void SystemCounters::incrementRestRequestsProcessed() { restRequestsProcessed++; }

void SystemCounters::incrementRtpEventsProcessed() { rtpEventsProcessed++; }

void SystemCounters::incrementRtpSendFailures() { rtpSendFailures++; }

void SystemCounters::incrementRtpSendFailuresSuppressed() { rtpSendFailuresSuppressed++; }

void SystemCounters::incrementRtpSendRecoveries() { rtpSendRecoveries++; }

void SystemCounters::incrementRtpCircuitBreakerTrips() { rtpCircuitBreakerTrips++; }

void SystemCounters::incrementRtcpReportsSent() { rtcpReportsSent++; }

void SystemCounters::incrementRtcpSendFailures() { rtcpSendFailures++; }

void SystemCounters::incrementSoundFilesServed() { soundFilesServed++; }

void SystemCounters::incrementWebsocketConnectionsProcessed() { websocketConnectionsProcessed++; }

void SystemCounters::incrementWebsocketMessagesReceived() { websocketMessagesReceived++; }

void SystemCounters::incrementWebsocketMessagesSent() { websocketMessagesSent++; }

void SystemCounters::incrementWebsocketPingsSent() { websocketPingsSent++; }

void SystemCounters::incrementWebsocketPongsReceived() { websocketPongsReceived++; }

void SystemCounters::incrementRtpEncoderResets() { rtpEncoderResets++; }

void SystemCounters::setRtpAudioLoadMetrics(uint64_t active, uint64_t queued, uint64_t accepted, uint64_t completed,
                                            uint64_t rejected, uint64_t cancelled, uint64_t failed) {
    rtpAudioLoadersActive.store(active);
    rtpAudioLoadsQueued.store(queued);
    rtpAudioLoadsAccepted.store(accepted);
    rtpAudioLoadsCompleted.store(completed);
    rtpAudioLoadsRejected.store(rejected);
    rtpAudioLoadsCancelled.store(cancelled);
    rtpAudioLoadsFailed.store(failed);
}

void SystemCounters::setLocalAudioPlaybackMetrics(uint64_t active, uint64_t queued, uint64_t accepted,
                                                  uint64_t completed, uint64_t replaced, uint64_t rejected,
                                                  uint64_t stopped, uint64_t failed, uint64_t timedOut) {
    localAudioPlaybacksActive.store(active);
    localAudioPlaybacksQueued.store(queued);
    localAudioPlaybacksAccepted.store(accepted);
    localAudioPlaybacksCompleted.store(completed);
    localAudioPlaybacksReplaced.store(replaced);
    localAudioPlaybacksRejected.store(rejected);
    localAudioPlaybacksStopped.store(stopped);
    localAudioPlaybacksFailed.store(failed);
    localAudioPlaybacksTimedOut.store(timedOut);
}

uint64_t SystemCounters::getTotalFrames() { return totalFrames.load(); }

uint64_t SystemCounters::getEventsProcessed() { return eventsProcessed.load(); }

uint64_t SystemCounters::getFramesStreamed() { return framesStreamed.load(); }

uint64_t SystemCounters::getDMXEventsProcessed() { return dmxEventsProcessed.load(); }

uint64_t SystemCounters::getAnimationsPlayed() { return animationsPlayed.load(); }

uint64_t SystemCounters::getSoundsPlayed() { return soundsPlayed.load(); }

uint64_t SystemCounters::getPlaylistsStarted() { return playlistsStarted.load(); }

uint64_t SystemCounters::getPlaylistsStopped() { return playlistsStopped.load(); }

uint64_t SystemCounters::getPlaylistsEventsProcessed() { return playlistsEventsProcessed.load(); }

uint64_t SystemCounters::getPlaylistStatusRequests() { return playlistStatusRequests.load(); }

uint64_t SystemCounters::getRestRequestsProcessed() { return restRequestsProcessed.load(); }

uint64_t SystemCounters::getRtpEventsProcessed() { return rtpEventsProcessed.load(); }

uint64_t SystemCounters::getRtpSendFailures() { return rtpSendFailures.load(); }

uint64_t SystemCounters::getRtpSendFailuresSuppressed() { return rtpSendFailuresSuppressed.load(); }

uint64_t SystemCounters::getRtpSendRecoveries() { return rtpSendRecoveries.load(); }

uint64_t SystemCounters::getRtpCircuitBreakerTrips() { return rtpCircuitBreakerTrips.load(); }

uint64_t SystemCounters::getRtcpReportsSent() { return rtcpReportsSent.load(); }

uint64_t SystemCounters::getRtcpSendFailures() { return rtcpSendFailures.load(); }

uint64_t SystemCounters::getRtpEncoderResets() { return rtpEncoderResets.load(); }

uint64_t SystemCounters::getRtpAudioLoadersActive() { return rtpAudioLoadersActive.load(); }

uint64_t SystemCounters::getRtpAudioLoadsQueued() { return rtpAudioLoadsQueued.load(); }

uint64_t SystemCounters::getRtpAudioLoadsAccepted() { return rtpAudioLoadsAccepted.load(); }

uint64_t SystemCounters::getRtpAudioLoadsCompleted() { return rtpAudioLoadsCompleted.load(); }

uint64_t SystemCounters::getRtpAudioLoadsRejected() { return rtpAudioLoadsRejected.load(); }

uint64_t SystemCounters::getRtpAudioLoadsCancelled() { return rtpAudioLoadsCancelled.load(); }

uint64_t SystemCounters::getRtpAudioLoadsFailed() { return rtpAudioLoadsFailed.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksActive() { return localAudioPlaybacksActive.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksQueued() { return localAudioPlaybacksQueued.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksAccepted() { return localAudioPlaybacksAccepted.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksCompleted() { return localAudioPlaybacksCompleted.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksReplaced() { return localAudioPlaybacksReplaced.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksRejected() { return localAudioPlaybacksRejected.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksStopped() { return localAudioPlaybacksStopped.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksFailed() { return localAudioPlaybacksFailed.load(); }

uint64_t SystemCounters::getLocalAudioPlaybacksTimedOut() { return localAudioPlaybacksTimedOut.load(); }

uint64_t SystemCounters::getSoundFilesServed() { return soundFilesServed.load(); }

uint64_t SystemCounters::getWebsocketConnectionsProcessed() { return websocketConnectionsProcessed.load(); }

uint64_t SystemCounters::getWebsocketMessagesReceived() { return websocketMessagesReceived.load(); }

uint64_t SystemCounters::getWebsocketMessagesSent() { return websocketMessagesSent.load(); }

uint64_t SystemCounters::getWebsocketPingsSent() { return websocketPingsSent.load(); }

uint64_t SystemCounters::getWebsocketPongsReceived() { return websocketPongsReceived.load(); }

SystemCountersSnapshot SystemCounters::snapshot() const {
    return {totalFrames.load(),
            eventsProcessed.load(),
            framesStreamed.load(),
            dmxEventsProcessed.load(),
            animationsPlayed.load(),
            soundsPlayed.load(),
            playlistsStarted.load(),
            playlistsStopped.load(),
            playlistsEventsProcessed.load(),
            playlistStatusRequests.load(),
            restRequestsProcessed.load(),
            rtpEventsProcessed.load(),
            rtpSendFailures.load(),
            rtpSendFailuresSuppressed.load(),
            rtpSendRecoveries.load(),
            rtpCircuitBreakerTrips.load(),
            rtcpReportsSent.load(),
            rtcpSendFailures.load(),
            rtpAudioLoadersActive.load(),
            rtpAudioLoadsQueued.load(),
            rtpAudioLoadsAccepted.load(),
            rtpAudioLoadsCompleted.load(),
            rtpAudioLoadsRejected.load(),
            rtpAudioLoadsCancelled.load(),
            rtpAudioLoadsFailed.load(),
            localAudioPlaybacksActive.load(),
            localAudioPlaybacksQueued.load(),
            localAudioPlaybacksAccepted.load(),
            localAudioPlaybacksCompleted.load(),
            localAudioPlaybacksReplaced.load(),
            localAudioPlaybacksRejected.load(),
            localAudioPlaybacksStopped.load(),
            localAudioPlaybacksFailed.load(),
            localAudioPlaybacksTimedOut.load(),
            soundFilesServed.load(),
            websocketConnectionsProcessed.load(),
            websocketMessagesReceived.load(),
            websocketMessagesSent.load(),
            websocketPingsSent.load(),
            websocketPongsReceived.load(),
            rtpEncoderResets.load()};
}

nlohmann::json systemCountersSnapshotToJson(const SystemCountersSnapshot &snapshot) {
    return {{"totalFrames", snapshot.totalFrames},
            {"eventsProcessed", snapshot.eventsProcessed},
            {"framesStreamed", snapshot.framesStreamed},
            {"dmxEventsProcessed", snapshot.dmxEventsProcessed},
            {"animationsPlayed", snapshot.animationsPlayed},
            {"soundsPlayed", snapshot.soundsPlayed},
            {"playlistsStarted", snapshot.playlistsStarted},
            {"playlistsStopped", snapshot.playlistsStopped},
            {"playlistsEventsProcessed", snapshot.playlistsEventsProcessed},
            {"playlistStatusRequests", snapshot.playlistStatusRequests},
            {"restRequestsProcessed", snapshot.restRequestsProcessed},
            {"rtpEventsProcessed", snapshot.rtpEventsProcessed},
            {"rtpSendFailures", snapshot.rtpSendFailures},
            {"rtpSendFailuresSuppressed", snapshot.rtpSendFailuresSuppressed},
            {"rtpSendRecoveries", snapshot.rtpSendRecoveries},
            {"rtpCircuitBreakerTrips", snapshot.rtpCircuitBreakerTrips},
            {"rtcpReportsSent", snapshot.rtcpReportsSent},
            {"rtcpSendFailures", snapshot.rtcpSendFailures},
            {"rtpAudioLoadersActive", snapshot.rtpAudioLoadersActive},
            {"rtpAudioLoadsQueued", snapshot.rtpAudioLoadsQueued},
            {"rtpAudioLoadsAccepted", snapshot.rtpAudioLoadsAccepted},
            {"rtpAudioLoadsCompleted", snapshot.rtpAudioLoadsCompleted},
            {"rtpAudioLoadsRejected", snapshot.rtpAudioLoadsRejected},
            {"rtpAudioLoadsCancelled", snapshot.rtpAudioLoadsCancelled},
            {"rtpAudioLoadsFailed", snapshot.rtpAudioLoadsFailed},
            {"localAudioPlaybacksActive", snapshot.localAudioPlaybacksActive},
            {"localAudioPlaybacksQueued", snapshot.localAudioPlaybacksQueued},
            {"localAudioPlaybacksAccepted", snapshot.localAudioPlaybacksAccepted},
            {"localAudioPlaybacksCompleted", snapshot.localAudioPlaybacksCompleted},
            {"localAudioPlaybacksReplaced", snapshot.localAudioPlaybacksReplaced},
            {"localAudioPlaybacksRejected", snapshot.localAudioPlaybacksRejected},
            {"localAudioPlaybacksStopped", snapshot.localAudioPlaybacksStopped},
            {"localAudioPlaybacksFailed", snapshot.localAudioPlaybacksFailed},
            {"localAudioPlaybacksTimedOut", snapshot.localAudioPlaybacksTimedOut},
            {"soundFilesServed", snapshot.soundFilesServed},
            {"websocketConnectionsProcessed", snapshot.websocketConnectionsProcessed},
            {"websocketMessagesReceived", snapshot.websocketMessagesReceived},
            {"websocketMessagesSent", snapshot.websocketMessagesSent},
            {"websocketPingsSent", snapshot.websocketPingsSent},
            {"websocketPongsReceived", snapshot.websocketPongsReceived},
            {"rtpEncoderResets", snapshot.rtpEncoderResets}};
}

oatpp::Object<SystemCountersDto> systemCountersToDto(const SystemCounters &counters) {
    const auto snapshot = counters.snapshot();
    auto dto = SystemCountersDto::createShared();
    dto->totalFrames = snapshot.totalFrames;
    dto->eventsProcessed = snapshot.eventsProcessed;
    dto->framesStreamed = snapshot.framesStreamed;
    dto->dmxEventsProcessed = snapshot.dmxEventsProcessed;
    dto->animationsPlayed = snapshot.animationsPlayed;
    dto->soundsPlayed = snapshot.soundsPlayed;
    dto->playlistsStarted = snapshot.playlistsStarted;
    dto->playlistsStopped = snapshot.playlistsStopped;
    dto->playlistsEventsProcessed = snapshot.playlistsEventsProcessed;
    dto->playlistStatusRequests = snapshot.playlistStatusRequests;
    dto->restRequestsProcessed = snapshot.restRequestsProcessed;
    dto->rtpEventsProcessed = snapshot.rtpEventsProcessed;
    dto->rtpSendFailures = snapshot.rtpSendFailures;
    dto->rtpSendFailuresSuppressed = snapshot.rtpSendFailuresSuppressed;
    dto->rtpSendRecoveries = snapshot.rtpSendRecoveries;
    dto->rtpCircuitBreakerTrips = snapshot.rtpCircuitBreakerTrips;
    dto->rtcpReportsSent = snapshot.rtcpReportsSent;
    dto->rtcpSendFailures = snapshot.rtcpSendFailures;
    dto->rtpEncoderResets = snapshot.rtpEncoderResets;
    dto->rtpAudioLoadersActive = snapshot.rtpAudioLoadersActive;
    dto->rtpAudioLoadsQueued = snapshot.rtpAudioLoadsQueued;
    dto->rtpAudioLoadsAccepted = snapshot.rtpAudioLoadsAccepted;
    dto->rtpAudioLoadsCompleted = snapshot.rtpAudioLoadsCompleted;
    dto->rtpAudioLoadsRejected = snapshot.rtpAudioLoadsRejected;
    dto->rtpAudioLoadsCancelled = snapshot.rtpAudioLoadsCancelled;
    dto->rtpAudioLoadsFailed = snapshot.rtpAudioLoadsFailed;
    dto->localAudioPlaybacksActive = snapshot.localAudioPlaybacksActive;
    dto->localAudioPlaybacksQueued = snapshot.localAudioPlaybacksQueued;
    dto->localAudioPlaybacksAccepted = snapshot.localAudioPlaybacksAccepted;
    dto->localAudioPlaybacksCompleted = snapshot.localAudioPlaybacksCompleted;
    dto->localAudioPlaybacksReplaced = snapshot.localAudioPlaybacksReplaced;
    dto->localAudioPlaybacksRejected = snapshot.localAudioPlaybacksRejected;
    dto->localAudioPlaybacksStopped = snapshot.localAudioPlaybacksStopped;
    dto->localAudioPlaybacksFailed = snapshot.localAudioPlaybacksFailed;
    dto->localAudioPlaybacksTimedOut = snapshot.localAudioPlaybacksTimedOut;
    dto->soundFilesServed = snapshot.soundFilesServed;
    dto->websocketConnectionsProcessed = snapshot.websocketConnectionsProcessed;
    dto->websocketMessagesReceived = snapshot.websocketMessagesReceived;
    dto->websocketMessagesSent = snapshot.websocketMessagesSent;
    dto->websocketPingsSent = snapshot.websocketPingsSent;
    dto->websocketPongsReceived = snapshot.websocketPongsReceived;
    return dto;
}
} // namespace creatures
