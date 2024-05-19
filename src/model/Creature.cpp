

#include <spdlog/spdlog.h>

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>


#include "Creature.h"

namespace creatures {


    // Convert a CreatureDto to a Creature
    Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto) {

        debug("Converting CreatureDto to Creature");

        Creature creature;
        creature.id = creatureDto->id;
        debug("id: {}", creature.id);

        creature.name = creatureDto->name;
        debug("name: {}", creature.name);

        creature.channel_offset = creatureDto->channel_offset;
        debug("channel_offset: {}", creature.channel_offset);

        creature.audio_channel = creatureDto->audio_channel;
        debug("audio_channel: {}", creature.audio_channel);

        return creature;
    }


    oatpp::Object<CreatureDto> convertToDto(const Creature &creature) {
        auto creatureDto = CreatureDto::createShared();
        creatureDto->id = creature.id;
        creatureDto->name = creature.name;
        creatureDto->channel_offset = creature.channel_offset;
        creatureDto->audio_channel = creature.audio_channel;

        return creatureDto;
    }

}