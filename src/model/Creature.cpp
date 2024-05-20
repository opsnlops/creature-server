

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

        // Make sure we're not about to read a null pointer
        if (creatureDto->inputs) {
            for (const auto &inputDto: *creatureDto->inputs) {
                Input newInput;
                newInput.name = inputDto->name;
                newInput.slot = inputDto->slot;
                newInput.width = inputDto->width;
                newInput.joystick_axis = inputDto->joystick_axis;

                creature.inputs.push_back(newInput);
            }
        }

        return creature;
    }


    oatpp::Object<CreatureDto> convertToDto(const Creature &creature) {
        auto creatureDto = CreatureDto::createShared();
        creatureDto->id = creature.id;
        creatureDto->name = creature.name;
        creatureDto->channel_offset = creature.channel_offset;
        creatureDto->audio_channel = creature.audio_channel;
        creatureDto->inputs = oatpp::List<oatpp::Object<InputDto>>::createShared();

        for( const auto& input : creature.inputs ) {
            auto inputDto = InputDto::createShared();
            inputDto->name = input.name;
            inputDto->slot = input.slot;
            inputDto->width = input.width;
            inputDto->joystick_axis = input.joystick_axis;

            creatureDto->inputs->push_back(inputDto);
        }

        return creatureDto;
    }

}