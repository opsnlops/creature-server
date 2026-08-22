

#include <spdlog/spdlog.h>

#include <algorithm>
#include <optional>

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "Creature.h"

namespace creatures {

// List of required fields
std::vector<std::string> creature_required_top_level_fields = {"id", "name", "audio_channel", "channel_offset",
                                                               "mouth_slot"};

std::vector<std::string> creature_required_input_fields = {"name", "slot", "width", "joystick_axis"};

namespace {

// Gaze axes (#119) are optional all the way down: an axis object may be
// present but missing a field. Anything unset falls back to a zero-width
// range, which the gaze layer treats as "don't drive this axis" rather than
// aiming somewhere unexpected.
GazeAxis convertGazeAxisFromDto(const oatpp::Object<GazeAxisDto> &axisDto) {
    GazeAxis axis{};
    if (axisDto->input) {
        axis.input = *axisDto->input;
    }
    axis.degrees_at_min = axisDto->degrees_at_min ? *axisDto->degrees_at_min : 0.0f;
    axis.degrees_at_max = axisDto->degrees_at_max ? *axisDto->degrees_at_max : 0.0f;
    if (axisDto->listening_amount) {
        axis.listening_amount = *axisDto->listening_amount;
    }
    return axis;
}

oatpp::Object<GazeAxisDto> convertGazeAxisToDto(const GazeAxis &axis) {
    auto axisDto = GazeAxisDto::createShared();
    axisDto->input = axis.input;
    axisDto->degrees_at_min = axis.degrees_at_min;
    axisDto->degrees_at_max = axis.degrees_at_max;
    axisDto->listening_amount = axis.listening_amount;
    return axisDto;
}

} // namespace

std::optional<uint16_t> inputSlotByName(const Creature &creature, const std::string &inputName) {
    const auto it = std::find_if(creature.inputs.begin(), creature.inputs.end(),
                                 [&](const Input &input) { return input.name == inputName; });
    if (it == creature.inputs.end()) {
        return std::nullopt;
    }
    return it->slot;
}

uint16_t resolvedMouthSlot(const Creature &creature) {
    if (!creature.mouth_input.empty()) {
        if (const auto slot = inputSlotByName(creature, creature.mouth_input)) {
            return *slot;
        }
        // Named an input this creature doesn't have. The parser rejects that
        // on upload, so falling back to the raw number is the safest thing a
        // stored document that drifted can do.
        warn("Creature '{}' mouth_input '{}' does not name one of its inputs; falling back to mouth_slot {}",
             creature.name, creature.mouth_input, static_cast<int>(creature.mouth_slot));
    }
    return creature.mouth_slot;
}

std::optional<bool> mouthSlotMatchesBeak(const Creature &creature) {
    const auto beakSlot = inputSlotByName(creature, "beak");
    if (!beakSlot) {
        return std::nullopt;
    }
    return resolvedMouthSlot(creature) == *beakSlot;
}

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

    if (creatureDto->mouth_input) {
        creature.mouth_input = *creatureDto->mouth_input;
    }

    if (creatureDto->speech_loop_animation_ids) {
        for (const auto &animationId : *creatureDto->speech_loop_animation_ids) {
            if (animationId) {
                creature.speech_loop_animation_ids.emplace_back(std::string(animationId));
            }
        }
        debug("speech_loop_animation_ids count: {}", creature.speech_loop_animation_ids.size());
    }

    if (creatureDto->idle_animation_ids) {
        for (const auto &animationId : *creatureDto->idle_animation_ids) {
            if (animationId) {
                creature.idle_animation_ids.emplace_back(std::string(animationId));
            }
        }
        debug("idle_animation_ids count: {}", creature.idle_animation_ids.size());
    }

    if (creatureDto->gaze) {
        GazeConfig gaze;
        if (creatureDto->gaze->pan) {
            gaze.pan = convertGazeAxisFromDto(creatureDto->gaze->pan);
        }
        if (creatureDto->gaze->elevation) {
            gaze.elevation = convertGazeAxisFromDto(creatureDto->gaze->elevation);
        }
        if (creatureDto->gaze->cock) {
            gaze.cock = convertGazeAxisFromDto(creatureDto->gaze->cock);
        }
        creature.gaze = gaze;
    }

    // Make sure we're not about to read a null pointer
    if (creatureDto->inputs) {
        for (const auto &inputDto : *creatureDto->inputs) {
            creature.inputs.push_back(convertFromDto(inputDto));
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
    if (!creature.mouth_input.empty()) {
        creatureDto->mouth_input = creature.mouth_input;
    }
    creatureDto->inputs = oatpp::List<oatpp::Object<InputDto>>::createShared();

    for (const auto &input : creature.inputs)
        creatureDto->inputs->push_back(convertToDto(input));

    if (!creature.speech_loop_animation_ids.empty()) {
        auto animations = oatpp::List<oatpp::String>::createShared();
        for (const auto &animationId : creature.speech_loop_animation_ids) {
            animations->push_back(animationId.c_str());
        }
        creatureDto->speech_loop_animation_ids = animations;
    }

    if (!creature.idle_animation_ids.empty()) {
        auto animations = oatpp::List<oatpp::String>::createShared();
        for (const auto &animationId : creature.idle_animation_ids) {
            animations->push_back(animationId.c_str());
        }
        creatureDto->idle_animation_ids = animations;
    }

    // Omitted entirely when unset, matching how the animation-id lists behave —
    // a creature config that never had a gaze block round-trips unchanged.
    if (creature.gaze) {
        auto gazeDto = GazeConfigDto::createShared();
        if (creature.gaze->pan) {
            gazeDto->pan = convertGazeAxisToDto(*creature.gaze->pan);
        }
        if (creature.gaze->elevation) {
            gazeDto->elevation = convertGazeAxisToDto(*creature.gaze->elevation);
        }
        if (creature.gaze->cock) {
            gazeDto->cock = convertGazeAxisToDto(*creature.gaze->cock);
        }
        creatureDto->gaze = gazeDto;
    }

    return creatureDto;
}

} // namespace creatures
