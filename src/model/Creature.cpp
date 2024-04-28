

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>


#include "Creature.h"

namespace creatures {


    // Convert a CreatureDto to a Creature
    Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto) {
        Creature creature;
        creature.id = creatureDto->id;
        creature.name = creatureDto->name;
        creature.channel_offset = creatureDto->channel_offset;
        creature.audio_channel = creatureDto->audio_channel;
        creature.notes = creatureDto->notes;

        return creature;
    }


    std::shared_ptr<CreatureDto> convertToDto(const Creature &creature) {
        auto creatureDto = CreatureDto::createShared();
        creatureDto->id = creature.id;
        creatureDto->name = creature.name;
        creatureDto->channel_offset = creature.channel_offset;
        creatureDto->audio_channel = creature.audio_channel;
        creatureDto->notes = creature.notes;

        return creatureDto.getPtr();
    }

}