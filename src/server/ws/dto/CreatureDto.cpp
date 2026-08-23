#include "CreatureDto.h"

namespace creatures {

namespace {

GazeAxis convertGazeAxisFromDto(const oatpp::Object<GazeAxisDto> &axisDto) {
    GazeAxis axis{};
    if (axisDto->input)
        axis.input = *axisDto->input;
    axis.degrees_at_min = axisDto->degrees_at_min ? *axisDto->degrees_at_min : 0.0f;
    axis.degrees_at_max = axisDto->degrees_at_max ? *axisDto->degrees_at_max : 0.0f;
    if (axisDto->listening_amount)
        axis.listening_amount = *axisDto->listening_amount;
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

Creature convertFromDto(const std::shared_ptr<CreatureDto> &creatureDto) {
    Creature creature;
    creature.id = creatureDto->id;
    creature.name = creatureDto->name;
    creature.channel_offset = creatureDto->channel_offset;
    creature.audio_channel = creatureDto->audio_channel;
    creature.mouth_slot = creatureDto->mouth_slot;
    if (creatureDto->mouth_input)
        creature.mouth_input = *creatureDto->mouth_input;

    if (creatureDto->speech_loop_animation_ids) {
        for (const auto &animationId : *creatureDto->speech_loop_animation_ids) {
            if (animationId)
                creature.speech_loop_animation_ids.emplace_back(animationId);
        }
    }
    if (creatureDto->idle_animation_ids) {
        for (const auto &animationId : *creatureDto->idle_animation_ids) {
            if (animationId)
                creature.idle_animation_ids.emplace_back(animationId);
        }
    }
    if (creatureDto->gaze) {
        GazeConfig gaze;
        if (creatureDto->gaze->pan)
            gaze.pan = convertGazeAxisFromDto(creatureDto->gaze->pan);
        if (creatureDto->gaze->elevation)
            gaze.elevation = convertGazeAxisFromDto(creatureDto->gaze->elevation);
        if (creatureDto->gaze->cock)
            gaze.cock = convertGazeAxisFromDto(creatureDto->gaze->cock);
        creature.gaze = gaze;
    }
    if (creatureDto->inputs) {
        for (const auto &inputDto : *creatureDto->inputs) {
            if (inputDto)
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
    if (!creature.mouth_input.empty())
        creatureDto->mouth_input = creature.mouth_input;

    creatureDto->inputs = oatpp::List<oatpp::Object<InputDto>>::createShared();
    for (const auto &input : creature.inputs)
        creatureDto->inputs->push_back(convertToDto(input));

    if (!creature.speech_loop_animation_ids.empty()) {
        auto animations = oatpp::List<oatpp::String>::createShared();
        for (const auto &animationId : creature.speech_loop_animation_ids)
            animations->push_back(animationId.c_str());
        creatureDto->speech_loop_animation_ids = animations;
    }
    if (!creature.idle_animation_ids.empty()) {
        auto animations = oatpp::List<oatpp::String>::createShared();
        for (const auto &animationId : creature.idle_animation_ids)
            animations->push_back(animationId.c_str());
        creatureDto->idle_animation_ids = animations;
    }
    if (creature.gaze) {
        auto gazeDto = GazeConfigDto::createShared();
        if (creature.gaze->pan)
            gazeDto->pan = convertGazeAxisToDto(*creature.gaze->pan);
        if (creature.gaze->elevation)
            gazeDto->elevation = convertGazeAxisToDto(*creature.gaze->elevation);
        if (creature.gaze->cock)
            gazeDto->cock = convertGazeAxisToDto(*creature.gaze->cock);
        creatureDto->gaze = gazeDto;
    }
    return creatureDto;
}

} // namespace creatures
