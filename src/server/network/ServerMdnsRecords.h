#pragma once

#include <cstdint>
#include <string>

#include "creature/network/MdnsService.h"
#include "server/config/Configuration.h"

namespace creatures::network {

/** DNS-SD service type for a running creature server */
inline constexpr const char *CREATURE_SERVER_SERVICE_TYPE = "_creature-server._tcp";

/** The API base path every server serves, advertised so nothing has to guess it */
inline constexpr const char *CREATURE_SERVER_API_BASE = "/api/v1";

/**
 * Everything about this running server that goes into the advertisement
 *
 * A plain struct rather than the Configuration itself, so the record builder is
 * a pure function of its inputs: Configuration's setters are protected for
 * CommandLine's benefit, which would make varying them in a test awkward.
 */
struct ServerDetails {
    std::string name;    ///< Instance name to advertise; the host name by default
    std::string version; ///< This server's own version
    uint16_t port = 0;   ///< The HTTP port actually bound, not the configured one

    bool travelMode = false; ///< Travel server (one Pi) rather than mainstage
    bool rtpAudio = true;    ///< Streaming over RTP rather than playing locally

    uint8_t audioChannels = 0;
    uint32_t audioSampleRate = 0;
    std::string audioDevice; ///< Local mode only; empty means the platform default

    std::string soundLibrary;
    std::string lipSyncEngine;
    uint16_t networkInterface = 0; ///< E1.31 interface index
};

/**
 * Collect the advertisement details from the server's configuration
 *
 * @param config the running configuration
 * @param name the instance name to advertise
 * @param version this server's version
 * @param port the HTTP port that was actually bound
 */
ServerDetails serverDetailsFrom(const Configuration &config, std::string name, std::string version, uint16_t port);

/**
 * The instance name to advertise: this host's name
 *
 * Falls back to "creature-server" if the host name can't be read, so there is
 * always something to publish. Not yet configurable — the host name is the
 * useful answer on both the mainstage and travel servers, and a config knob can
 * come along the first time it isn't.
 */
std::string serverMdnsName();

/**
 * Describe this running server for mDNS
 */
MdnsService makeServerService(const ServerDetails &details);

} // namespace creatures::network
