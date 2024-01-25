
#pragma once

#include <array>
#include <mutex>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#define UNIVERSE_SLOT_COUNT 512

namespace creatures::e131 {

    class Universe {

    public:
        Universe(std::shared_ptr<spdlog::logger> logger);
        ~Universe() = default;

        template <size_t N>
        void setFragment(uint16_t firstSlot, std::array<uint8_t, N>& values);

        std::array<uint8_t, UNIVERSE_SLOT_COUNT> getState();

        uint8_t getNextSequenceNumber();

    private:

        // Keep track of the sequence number we're currently on
        uint8_t sequenceNumber = 0;

        // The state of the universe as we know it
        std::array<uint8_t, 512> state = {};

        // Make sure only one thread touches the values at once
        std::mutex valuesMutex;

        // 🪵
        std::shared_ptr<spdlog::logger> logger;

    };

} // creatures::e131


