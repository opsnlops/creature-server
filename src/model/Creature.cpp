

#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "Creature.h"

namespace creatures {

// List of required fields
std::vector<std::string> creature_required_top_level_fields = {"id", "name", "audio_channel", "channel_offset",
                                                                "mouth_slot"};

std::vector<std::string> creature_required_input_fields = {"name", "slot", "width", "joystick_axis"};

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

    creature.mouth_slot = creatureDto->mouth_slot;
    debug("mouth_slot: {}", static_cast<int>(creature.mouth_slot));

    // Make sure we're not about to read a null pointer
    if (creatureDto->inputs) {
        for (const auto &inputDto : *creatureDto->inputs) {
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
    creatureDto->mouth_slot = creature.mouth_slot;
    creatureDto->inputs = oatpp::List<oatpp::Object<InputDto>>::createShared();

    for (const auto &[name, slot, width, joystick_axis] : creature.inputs) {
        auto inputDto = InputDto::createShared();
        inputDto->name = name;
        inputDto->slot = slot;
        inputDto->width = width;
        inputDto->joystick_axis = joystick_axis;

        creatureDto->inputs->push_back(inputDto);
    }

    return creatureDto;
}

} // namespace creatures