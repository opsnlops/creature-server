
#pragma once

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <uuid/uuid.h>



extern "C" {
    #include <e131.h>
}

namespace creatures::e131 {

    class E131Server {

    public:

        E131Server() = default;
        ~E131Server() = default;

        void init(uint16_t networkDevice);
        void start();

    private:
        std::shared_ptr<spdlog::logger> logger;

        const uint16_t universeNumber = 1;

        // 512 channels plus the START code (slot 0)
        std::array<uint8_t, 512> universeState = {};

        std::thread worker;
        void workerTask();

        uuid_t cid;
        int socket;

        uint16_t networkDevice;
    };

} // creatures::e131

