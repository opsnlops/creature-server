
#pragma once

#include <chrono>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

    struct Creature {

        /**
         * The ID of the creature
         */
        creatureId_t id;

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

class CreatureDto : public oatpp::DTO {

    DTO_INIT(CreatureDto, DTO /* extends */)

    DTO_FIELD_INFO(id) {
        info->description = "Creature ID in the form of a MongoDB OID";
    }

    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) {
        info->description = "The creature's name";
    }

    DTO_FIELD(String, name);

    DTO_FIELD_INFO(channel_offset) {
        info->description = "The offset of the channel for this creature in the universe";
    }

    DTO_FIELD(UInt16, channel_offset);

    DTO_FIELD_INFO(audio_channel) {
        info->description = "The audio channel for this creature";
    }

    DTO_FIELD(UInt16, audio_channel);

    DTO_FIELD_INFO(notes) {
        info->description = "A general notes field for the creature";
    }

    DTO_FIELD(String, notes);

};

#include OATPP_CODEGEN_END(DTO)

    oatpp::Object<CreatureDto> convertToDto(const Creature &creature);

    Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto);

}