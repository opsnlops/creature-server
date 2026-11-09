#include <array>
#include <string>
#include <utility>

#include <unistd.h>

#include "server/network/ServerMdnsRecords.h"

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

    details.audioChannels = config.getSoundChannels();
    details.audioSampleRate = config.getSoundFrequency();
    details.audioDevice = config.getSoundDeviceName().value_or("");

    details.soundLibrary = config.getSoundFileLocation();
    details.lipSyncEngine = config.getLipSyncEngine();
    details.networkInterface = config.getNetworkDevice();

    return details;
}

MdnsService makeServerService(const ServerDetails &details) {
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

    return service;
}

} // namespace creatures::network
