
#include "counters.h"

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

/**
 * Create a DTO from the current state of the counters
 *
 * @return a shared pointer to the DTO
 */
oatpp::Object<SystemCountersDto> SystemCounters::convertToDto() {

    auto dto = SystemCountersDto::createShared();

    dto->totalFrames = totalFrames.load();
    dto->eventsProcessed = eventsProcessed.load();
    dto->framesStreamed = framesStreamed.load();
    dto->dmxEventsProcessed = dmxEventsProcessed.load();
    dto->animationsPlayed = animationsPlayed.load();
    dto->soundsPlayed = soundsPlayed.load();
    dto->playlistsStarted = playlistsStarted.load();
    dto->playlistsStopped = playlistsStopped.load();
    dto->playlistsEventsProcessed = playlistsEventsProcessed.load();
    dto->playlistStatusRequests = playlistStatusRequests.load();
    dto->restRequestsProcessed = restRequestsProcessed.load();
    dto->rtpEventsProcessed = rtpEventsProcessed.load();
    dto->rtpSendFailures = rtpSendFailures.load();
    dto->rtpEncoderResets = rtpEncoderResets.load();
    dto->rtpAudioLoadersActive = rtpAudioLoadersActive.load();
    dto->rtpAudioLoadsQueued = rtpAudioLoadsQueued.load();
    dto->rtpAudioLoadsAccepted = rtpAudioLoadsAccepted.load();
    dto->rtpAudioLoadsCompleted = rtpAudioLoadsCompleted.load();
    dto->rtpAudioLoadsRejected = rtpAudioLoadsRejected.load();
    dto->rtpAudioLoadsCancelled = rtpAudioLoadsCancelled.load();
    dto->rtpAudioLoadsFailed = rtpAudioLoadsFailed.load();
    dto->localAudioPlaybacksActive = localAudioPlaybacksActive.load();
    dto->localAudioPlaybacksQueued = localAudioPlaybacksQueued.load();
    dto->localAudioPlaybacksAccepted = localAudioPlaybacksAccepted.load();
    dto->localAudioPlaybacksCompleted = localAudioPlaybacksCompleted.load();
    dto->localAudioPlaybacksReplaced = localAudioPlaybacksReplaced.load();
    dto->localAudioPlaybacksRejected = localAudioPlaybacksRejected.load();
    dto->localAudioPlaybacksStopped = localAudioPlaybacksStopped.load();
    dto->localAudioPlaybacksFailed = localAudioPlaybacksFailed.load();
    dto->localAudioPlaybacksTimedOut = localAudioPlaybacksTimedOut.load();
    dto->websocketConnectionsProcessed = websocketConnectionsProcessed.load();
    dto->websocketMessagesReceived = websocketMessagesReceived.load();
    dto->websocketMessagesSent = websocketMessagesSent.load();
    dto->websocketPingsSent = websocketPingsSent.load();
    dto->websocketPongsReceived = websocketPongsReceived.load();

    return dto;
}
} // namespace creatures
