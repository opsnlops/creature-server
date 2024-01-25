
#include <chrono>
#include <thread>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <uuid/uuid.h>

// Junk to make random values
#include <array>
#include <random>
#include <algorithm>
#include <cstddef>

extern "C" {
    #include <e131.h>
}


#include "E131Server.h"

namespace creatures::e131 {


    void E131Server::init(uint16_t _networkDevice) {

        // Get our logger going
        logger = spdlog::stdout_color_mt("E131Server");
        logger->set_level(spdlog::level::trace);

        logger->debug("The state of the universe has {} slots", this->universeState.size());

        this->networkDevice = _networkDevice;
        logger->debug("using network device {}", this->networkDevice);

        // TODO: This is all temp
        if ((socket = e131_socket()) < 0) {
            logger->critical( "e131_socket: {}", strerror(errno));
        }

        if (e131_multicast_iface(socket, this->networkDevice) < 0) {
            logger->critical( "e131_multicast_join: {}", strerror(errno));
        }


        uuid_generate(cid);
        char cid_str[37];
        uuid_unparse(cid, cid_str);
        logger->info("CID: {}", cid_str);



        logger->info("E131Server Initialized");

    }

    void E131Server::start() {

        logger->debug("starting the E131Server");


        // Start our worker thread
        this->worker = std::thread(&E131Server::workerTask, this);
        worker.detach();
    }

    void E131Server::workerTask() {


        // Random number engine
        std::random_device rd;  // Seed
        std::mt19937 gen(rd()); // Mersenne Twister engine

        // Distribution for uint8_t
        std::uniform_int_distribution<uint8_t> distrib(0, 255);

        uint8_t seq = 0;

        while (true) {

            // Fill the universe with chaos
            for (auto& val : universeState) {
                val = distrib(gen);
            }

            e131_packet_t packet;
            e131_addr_t dest;

            e131_pkt_init(&packet, universeNumber, universeState.size());
            e131_multicast_dest(&dest, universeNumber, E131_DEFAULT_PORT);

            // Copy our cid
            std::copy(std::begin(cid), std::end(cid), std::begin(packet.root.cid));
            std::copy(universeState.begin(), universeState.end(), packet.dmp.prop_val);

            // Make sure we don't set the START code to anything other than zero
            packet.dmp.prop_val[0] = 0;
            packet.frame.seq_number = seq++;

            if (e131_send(socket, &packet, &dest) < 0) {
                logger->critical( "e131_send: {}", strerror(errno));
            }

            logger->trace("tick");
            std::this_thread::sleep_for(std::chrono::milliseconds (100));
        }

    }


} // creatures::e131
