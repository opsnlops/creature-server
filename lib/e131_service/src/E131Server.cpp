



#include <chrono>
#include <climits>
#include <thread>
#include <unistd.h>

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


    void E131Server::init(uint16_t _networkDevice, std::string _version) {

        // Get our logger going
        logger = spdlog::stdout_color_mt("E131Server");
        logger->set_level(spdlog::level::trace);

        this->networkDevice = _networkDevice;
        logger->debug("using network device {}", this->networkDevice);

        this->version = std::move(_version);
        logger->debug("using version {}", this->version);

        // Let's figure out our source name
        char hostname[65];
        memset(hostname, '\0', sizeof(hostname));

        if (gethostname(hostname, sizeof(hostname)) < 0) {
            logger->error("Unable to get host name: {}", strerror(errno));
            strcpy(hostname, "unknown");
        }

        // Set up our source name, being careful to not exceed the length limit
        logger->debug("hostname: {}", hostname);
        std::string sourceHeader = fmt::format("creature-server v{} on {}", this->version, hostname);
        memcpy(this->sourceName, sourceHeader.c_str(), sourceHeader.size() < SOURCE_NAME_LENGTH ? sourceHeader.size() : SOURCE_NAME_LENGTH);

        logger->debug("source header: {}, size: {}", sourceHeader, sourceHeader.size());

        // Create our socket for talking out to the E1.31 network
        if ((socket = e131_socket()) < 0) {
            logger->critical( "e131_socket: {}", strerror(errno));
        }

        // Join our multicast group
        if (e131_multicast_iface(socket, this->networkDevice) < 0) {
            logger->critical( "e131_multicast_join: {}", strerror(errno));
        }

        // Create our CID
        //
        //  The e1.31 spec says that the CID should be consistent for the lifetime of
        //  a device. Since this isn't commercial gear I'm selling I'll use a random
        //  one for now, but this might need to live in a config file if I wanted to
        //  actually meet the spec for gear that's not mine.
        //
        //  The actual creature-controllers don't care one bit. 😅
        //
        uuid_generate(cid);
        char cid_str[37];
        uuid_unparse(cid, cid_str);
        logger->debug("our cid: {}", cid_str);


        logger->info("E131Server Initialized");

    }

    void E131Server::start() {

        logger->debug("starting the E131Server");

        // Start our worker thread
        this->worker = std::thread(&E131Server::workerTask, this);
        worker.detach();
    }

    void E131Server::createUniverse(uint16_t universeNumber) {
        logger->info("creating universe {}", universeNumber);
        this->galaxy[universeNumber] = std::make_shared<Universe>(logger);
    }

    void E131Server::destroyUniverse(uint16_t universeNumber) {
        logger->info("destroying universe {}", universeNumber);
        this->galaxy.erase(universeNumber);
    }

    template <size_t N>
    void E131Server::setValues(uint16_t universeNumber, uint16_t firstSlot, std::array<uint8_t, N> &values) {

        auto universe = this->galaxy[universeNumber];
        if (universe) {
            universe->setFragment(firstSlot, values);
            logger->debug("set values starting at slot {} on universe {}", firstSlot, universeNumber);
        }
        else {
            logger->error("unable to set values on universe {} because it does not exist!", universeNumber);
        }
    }


    [[noreturn]] void E131Server::workerTask() {

        using namespace std::chrono;

        logger->info("hello from the worker task! 👋🏻");

        auto targetDelta = milliseconds(E131_FRAME_TIME_MS);
        auto nextTargetTime = high_resolution_clock::now() + targetDelta;


        while (true) {

            // Visit all of the universes in the galaxy
            for (const auto& pair : galaxy) {
                uint16_t universeNumber = pair.first;
                auto universe = pair.second;

                auto state = universe->getState();

                e131_packet_t packet;
                e131_addr_t dest;

                // Create a packet
                e131_pkt_init(&packet, universeNumber, state.size());
                e131_multicast_dest(&dest, universeNumber, E131_DEFAULT_PORT);

                // Copy our cid
                std::copy(std::begin(cid), std::end(cid), std::begin(packet.root.cid));
                std::copy(state.begin(), state.end(), packet.dmp.prop_val);

                // Copy our source name
                std::copy(std::begin(this->sourceName), std::end(this->sourceName), std::begin(packet.frame.source_name));

                // Make sure we don't set the START code to anything other than zero
                packet.dmp.prop_val[0] = 0;
                packet.frame.seq_number = universe->getNextSequenceNumber();

                // Toss it out on the network and hope it makes something happy 😍
                if (e131_send(socket, &packet, &dest) < 0) {
                    logger->critical("e131_send: {}", strerror(errno));
                }
            }

            // Leave a clue on what happened
            if(++frameCounter % 1000 == 0) {
                logger->debug("sent {} e1.31 frames", frameCounter);
            }

            // Figure out how much time we have until the next tick
            auto remainingTime = nextTargetTime - high_resolution_clock::now();

            // If there's time left, wait.
            if (remainingTime > nanoseconds(0)) {
                // Sleep for the remaining time
                std::this_thread::sleep_for(remainingTime);
            }

            // Update the target time for the next iteration
            nextTargetTime += targetDelta;
        }

    }


} // creatures::e131
