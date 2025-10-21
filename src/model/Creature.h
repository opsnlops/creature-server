
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "Input.h"

#include "server/namespace-stuffs.h"

/**
 * Note to myself for later!
 *
 * Don't get confused by `id` and `_id` in Mongo. `id` is what we use. It can be
 * basically any string. Normally it looks like an OID, but it doesn't have to.
 * It could be a UUID or whatever.
 */

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
     * The slot in the motion array that corresponds to the creature's mouth.
     * This is used by the Rhubarb Lip Sync system to automatically generate
     * mouth movements during animation.
     */
    uint8_t mouth_slot;

    /**
     * The inputs for this creature
     */
    std::vector<Input> inputs;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class CreatureDto : public oatpp::DTO {

    DTO_INIT(CreatureDto, DTO /* extends */)

    DTO_FIELD_INFO(id) { info->description = "Creature ID in the form of a MongoDB OID"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "The creature's name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(channel_offset) {
        info->description = "The offset of the channel for this creature in the universe";
    }
    DTO_FIELD(UInt16, channel_offset);

    DTO_FIELD_INFO(audio_channel) { info->description = "The audio channel for this creature"; }
    DTO_FIELD(UInt16, audio_channel);

    DTO_FIELD_INFO(mouth_slot) {
        info->description = "The slot in the motion array that corresponds to the creature's mouth";
        info->required = true;
    }
    DTO_FIELD(UInt8, mouth_slot);

    DTO_FIELD_INFO(inputs) { info->description = "The input map for this creature"; }
    DTO_FIELD(List<Object<InputDto>>, inputs);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<CreatureDto> convertToDto(const Creature &creature);
creatures::Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto);

} // namespace creatures