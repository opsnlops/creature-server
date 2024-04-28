
#pragma once

#include <chrono>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>


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

    };


#include OATPP_CODEGEN_BEGIN(DTO)

    class CreatureDTO : public oatpp::DTO {

        DTO_INIT(CreatureDTO, DTO /* extends */)

        DTO_FIELD(String, id);
        DTO_FIELD(String, name);
        DTO_FIELD(UInt16, channel_offset);
        DTO_FIELD(UInt16, audio_channel);
        DTO_FIELD(String, notes);

    };

#include OATPP_CODEGEN_END(DTO)

}