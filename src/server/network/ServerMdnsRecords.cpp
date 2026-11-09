#include <array>
#include <string>
#include <utility>

#include <unistd.h>

#include "server/network/ServerMdnsRecords.h"

#include "creature/rtp/Protocol.h"
#include "creature/timesync/TimeSync.h"

#include "server/config.h"

namespace creatures::network {

std::string serverMdnsName() {
    std::array<char, 256> host{};
    if (gethostname(host.data(), host.size() - 1) != 0 || host[0] == '\0') {
        return "creature-server";
    }
    return std::string(host.data());
}

ServerDetails serverDetailsFrom(const Configuration &config, std::string name, std::string version, uint16_t port) {
    ServerDetails details;
    details.name = std::move(name);
    details.version = std::move(version);
    details.port = port;

    details.travelMode = config.getTravelMode();
    details.rtpAudio = config.getAudioMode() == Configuration::AudioMode::RTP;

    // Report the audio this server actually produces, which depends on the mode.
    // In RTP mode that is the 17 multicast channels at the Opus rate; the local
    // sound_channels setting describes a device we are not using, and publishing
    // it here read as "this server streams 2 channels" on the main rig.
    if (details.rtpAudio) {
        details.audioChannels = creatures::rtp::CHANNEL_COUNT;
        details.audioSampleRate = creatures::rtp::SAMPLE_RATE;
    } else {
        details.audioChannels = config.getSoundChannels();
        details.audioSampleRate = config.getSoundFrequency();
    }
    details.audioDevice = config.getSoundDeviceName().value_or("");

    details.soundLibrary = config.getSoundFileLocation();
    details.lipSyncEngine = config.getLipSyncEngine();
    details.networkInterface = config.getNetworkDevice();

    return details;
}

MdnsService makeServerService(const ServerDetails &details, const timesync::TimeSyncStatus &timeSync) {
    MdnsService service;
    service.name = mdnsInstanceName(details.name);
    service.type = CREATURE_SERVER_SERVICE_TYPE;
    service.port = details.port;

    service.txt = {
        {"txtvers", "1"},
        {"version", details.version},
        {"api", CREATURE_SERVER_API_BASE},
        {"mode", details.travelMode ? "travel" : "mainstage"},
        {"audio", details.rtpAudio ? "rtp" : "local"},
        {"audio_channels", std::to_string(details.audioChannels)},
        {"audio_rate_hz", std::to_string(details.audioSampleRate)},
    };

    // Only one of these pairs is meaningful at a time, and advertising the other
    // would invite somebody to believe it.
    if (details.rtpAudio) {
        service.txt.emplace_back("rtp_port", std::to_string(creatures::rtp::RTP_PORT));
        service.txt.emplace_back("rtcp_port", std::to_string(creatures::rtp::RTCP_PORT));
    } else {
        service.txt.emplace_back("audio_device", details.audioDevice.empty() ? "default" : details.audioDevice);
    }

    service.txt.emplace_back("sound_library", details.soundLibrary);
    service.txt.emplace_back("lipsync", details.lipSyncEngine);
    service.txt.emplace_back("network_interface", std::to_string(details.networkInterface));

    // PTP state, the same keys the controllers and the audio router publish, so
    // one browse of the network shows whether every host agrees on the time and
    // on who the grandmaster is. Only once it has actually been checked:
    // SyncState::Unknown before the first poll would be a claim, not a reading.
    if (timeSync.state != timesync::SyncState::Unknown || timeSync.ptp.available) {
        timesync::appendTimeSyncTxt(service.txt, timeSync);
    }

    return service;
}

} // namespace creatures::network
