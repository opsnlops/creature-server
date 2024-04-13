
#pragma once

#include <chrono>
#include <string>

#include <nlohmann/json.hpp>

namespace creatures {

    struct Creature {

        /**
         * The ID of the creature
         */
        std::string id;

        /**
         * The name of the creature
         */
        std::string name;

        /**
         * The offset of the channel for this creature in the universe
         */
        uint16_t channel_offset;

        /**
         * The audio channel for this creature
         */
        uint16_t audio_channel;

        /**
         * Any notes to save in the database
         */
        std::string notes;

        NLOHMANN_DEFINE_TYPE_INTRUSIVE(Creature, id, name, channel_offset, audio_channel, notes)
    };
}