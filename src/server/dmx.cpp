
#include "spdlog/spdlog.h"

#include <string>
#include <utility>

#include "server/dmx.h"

using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

/**
 * This is a big mess
 *
 * I bootstrapped myself from the old creature-utils package
 */

namespace creatures {

    DMX::DMX() {

        trace("starting up a DMX sender");

        dmx_offset = 0;
        hostBanner = fmt::format("e1.31 client in the creature server");
        hostBannerLength = hostBanner.size();

        debug("set the host banner to: {}, len: {}", hostBanner, hostBannerLength);
    }


    void DMX::init(std::string client_ip, uint32_t dmx_universe, uint32_t numMotors) {

        this->number_of_motors = numMotors;
        this->universe = dmx_universe;
        this->ip_address = std::move(client_ip);

        debug("starting up the DMX client: motors: {}, universe: {}, ip: {}", number_of_motors, universe,ip_address);

        // create a socket for E1.31
        if ((socketFd = e131_socket()) < 0) {
            critical("Unable to open socket for the E1.31 client");
            return;
        }

        e131_pkt_init(&packet, universe, number_of_motors);
        memcpy(&packet.frame.source_name, hostBanner.c_str(), hostBannerLength);
        if (e131_set_option(&packet, E131_OPT_PREVIEW, false) < 0)
            error("unable to set e131 packet option");

        // Set the target
        if (e131_unicast_dest(&dest, ip_address.c_str(), E131_DEFAULT_PORT) < 0)
            error("unable to set e131 destination");

        // This is helpful for debugging
        //e131_pkt_dump(stdout, &packet);

        debug("socket created");
    }


    void DMX::send(uint8_t* data, uint8_t count) {

        trace("sending update");

        for (size_t pos = 0; pos < count; pos++) {
            packet.dmp.prop_val[pos + 1] = data[pos];
            trace("pos {}, data {}", (pos + dmx_offset + 1), data[pos]);
        }
        if (e131_send(socketFd, &packet, &dest) < 0) {
            error("unable to send e131 packet");
            e131_pkt_dump(stdout, &packet);
        }
        //e131_pkt_dump(stdout, &packet);
        packet.frame.seq_number++;

    }

    DMX::~DMX() = default;

}