
#pragma once

#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <uuid/uuid.h>

#include "Universe.h"

extern "C" {
    #include <e131.h>
}


#define E131_FRAME_TIME_MS  20
#define SOURCE_NAME_LENGTH  63  // The e1.31 spec says that the source name should be 64 bytes (63 + null terminator)

namespace creatures::e131 {

    class E131Server {

    public:

        E131Server() = default;
        ~E131Server() = default;

        void init(uint16_t _networkDevice, std::string _version);
        void start();

        // Add a new universe
        void createUniverse(uint16_t universeNumber);

        // Remove an existing universe
        void destroyUniverse(uint16_t universeNumber);

        void setValues(uint16_t universeNumber, uint16_t firstSlot, std::vector<uint8_t> &values);

    private:
        std::shared_ptr<spdlog::logger> logger;

        // All of our universes
        std::unordered_map<uint16_t, std::shared_ptr<Universe>> galaxy = {};

        std::thread worker;

        [[noreturn]] void workerTask();

        uuid_t cid;
        int socket;

        uint16_t networkDevice;

        uint64_t frameCounter = 0;

        std::string version;
        uint8_t sourceName[SOURCE_NAME_LENGTH] = { 0 };
    };

} // creatures::e131

