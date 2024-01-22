
#include "spdlog/spdlog.h"

#include <string>
#include <utility>

#include "dmx.h"
#include "server/config.h"


#include "server/namespace-stuffs.h"

namespace creatures {

    DMX::DMX() {

        trace("starting up a DMX sender");

        dmx_offset = 0;
        use_multicast = false;
        hostBanner = fmt::format("e1.31 client in the creature server");
        hostBannerLength = hostBanner.size();

        debug("set the host banner to: {}, len: {}", hostBanner, hostBannerLength);
    }


    uint8_t DMX::getSequenceNumber() const {
        return packet.frame.seq_number;
    }

    void DMX::init(std::string client_ip, bool useMulticast, uint32_t dmx_universe, uint32_t numMotors) {

        this->number_of_motors = numMotors;
        this->universe = dmx_universe;
        this->use_multicast = useMulticast;
        this->ip_address = std::move(client_ip);

        debug("starting up a DMX client: motors: {}, universe: {}, ip: {}", number_of_motors, universe,ip_address);

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
        if(!use_multicast) {
            debug("using unicast");
            if (e131_unicast_dest(&dest, ip_address.c_str(), E131_DEFAULT_PORT) < 0)
                error("unable to set e131 unicast destination");
        } else {
            debug("using multicast");
            if (e131_multicast_dest(&dest, dmx_universe, E131_DEFAULT_PORT) < 0)
                error("unable to set e131 multicast destination");
        }

#if DEBUG_DMX_SENDER
        // This is helpful for debugging
        e131_pkt_dump(stdout, &packet);
#endif
        debug("socket created");
    }

    /**
     * Send data to the target
     *
     * @param data a vector with the data to send
     */
    void DMX::send(const std::vector<uint8_t>& data) {

#if DEBUG_DMX_SENDER
        trace("sending update {}", packet.frame.seq_number);
#endif
        // Pack the vector into the array
        size_t count = data.size();
        for (size_t pos = 0; pos < count; pos++) {
            packet.dmp.prop_val[pos + 1] = data[pos];

#if DEBUG_DMX_SENDER
            trace("pos {}, data {}", (pos + dmx_offset + 1), data[pos]);
#endif
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