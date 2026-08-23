#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures {

class SystemCounters;

#include OATPP_CODEGEN_BEGIN(DTO)

class SystemCountersDto : public oatpp::DTO {
    DTO_INIT(SystemCountersDto, DTO)

    DTO_FIELD(UInt64, totalFrames);
    DTO_FIELD(UInt64, eventsProcessed);
    DTO_FIELD(UInt64, framesStreamed);
    DTO_FIELD(UInt64, dmxEventsProcessed);
    DTO_FIELD(UInt64, animationsPlayed);
    DTO_FIELD(UInt64, soundsPlayed);
    DTO_FIELD(UInt64, playlistsStarted);
    DTO_FIELD(UInt64, playlistsStopped);
    DTO_FIELD(UInt64, playlistsEventsProcessed);
    DTO_FIELD(UInt64, playlistStatusRequests);
    DTO_FIELD(UInt64, restRequestsProcessed);
    DTO_FIELD(UInt64, rtpEventsProcessed);
    DTO_FIELD(UInt64, rtpSendFailures);
    DTO_FIELD(UInt64, rtpSendFailuresSuppressed);
    DTO_FIELD(UInt64, rtpSendRecoveries);
    DTO_FIELD(UInt64, rtpCircuitBreakerTrips);
    DTO_FIELD(UInt64, rtcpReportsSent);
    DTO_FIELD(UInt64, rtcpSendFailures);
    DTO_FIELD(UInt64, rtpAudioLoadersActive);
    DTO_FIELD(UInt64, rtpAudioLoadsQueued);
    DTO_FIELD(UInt64, rtpAudioLoadsAccepted);
    DTO_FIELD(UInt64, rtpAudioLoadsCompleted);
    DTO_FIELD(UInt64, rtpAudioLoadsRejected);
    DTO_FIELD(UInt64, rtpAudioLoadsCancelled);
    DTO_FIELD(UInt64, rtpAudioLoadsFailed);
    DTO_FIELD(UInt64, localAudioPlaybacksActive);
    DTO_FIELD(UInt64, localAudioPlaybacksQueued);
    DTO_FIELD(UInt64, localAudioPlaybacksAccepted);
    DTO_FIELD(UInt64, localAudioPlaybacksCompleted);
    DTO_FIELD(UInt64, localAudioPlaybacksReplaced);
    DTO_FIELD(UInt64, localAudioPlaybacksRejected);
    DTO_FIELD(UInt64, localAudioPlaybacksStopped);
    DTO_FIELD(UInt64, localAudioPlaybacksFailed);
    DTO_FIELD(UInt64, localAudioPlaybacksTimedOut);
    DTO_FIELD(UInt64, soundFilesServed);
    DTO_FIELD(UInt64, websocketConnectionsProcessed);
    DTO_FIELD(UInt64, websocketMessagesReceived);
    DTO_FIELD(UInt64, websocketMessagesSent);
    DTO_FIELD(UInt64, websocketPingsSent);
    DTO_FIELD(UInt64, websocketPongsReceived);
    DTO_FIELD(UInt64, rtpEncoderResets);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<SystemCountersDto> systemCountersToDto(const SystemCounters &counters);

} // namespace creatures
