

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>

#include "AnimationMetadata.h"

namespace creatures {

/*
 *         DTO_FIELD(String, animation_id);
        DTO_FIELD(String, title);
        DTO_FIELD(UInt32, milliseconds_per_frame);
        DTO_FIELD(String, note);
        DTO_FIELD(String, sound_file);
        DTO_FIELD(UInt32, number_of_frames);
        DTO_FIELD(Boolean, multitrack_audio);
 */

// Convert a CreatureDto to a Creature
AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &animationMetadataDto) {
    AnimationMetadata metadata;
    metadata.animation_id = animationMetadataDto->animation_id;
    metadata.title = animationMetadataDto->title;
    metadata.milliseconds_per_frame = animationMetadataDto->milliseconds_per_frame;
    metadata.note = animationMetadataDto->note;
    metadata.sound_file = animationMetadataDto->sound_file;
    metadata.number_of_frames = animationMetadataDto->number_of_frames;
    metadata.multitrack_audio = animationMetadataDto->multitrack_audio;
    if (animationMetadataDto->source_stage_id) {
        metadata.source_stage_id = animationMetadataDto->source_stage_id;
    }
    if (animationMetadataDto->source_stage_updated_at) {
        metadata.source_stage_updated_at = *animationMetadataDto->source_stage_updated_at;
    }
    if (animationMetadataDto->render_seed) {
        metadata.render_seed = *animationMetadataDto->render_seed;
    }
    if (animationMetadataDto->source_render_choices) {
        for (const auto &choiceDto : *animationMetadataDto->source_render_choices) {
            if (!choiceDto) {
                continue;
            }
            CreatureRenderChoice choice;
            if (choiceDto->creature_id)
                choice.creature_id = choiceDto->creature_id;
            if (choiceDto->speech_loop_animation_id)
                choice.speech_loop_animation_id = choiceDto->speech_loop_animation_id;
            if (choiceDto->idle_animation_id)
                choice.idle_animation_id = choiceDto->idle_animation_id;
            if (choiceDto->idle_start_offset)
                choice.idle_start_offset = *choiceDto->idle_start_offset;
            metadata.source_render_choices.push_back(std::move(choice));
        }
    }
    if (animationMetadataDto->source_script_id) {
        metadata.source_script_id = animationMetadataDto->source_script_id;
    }
    if (animationMetadataDto->source_script_turns) {
        for (const auto &td : *animationMetadataDto->source_script_turns) {
            if (!td)
                continue;
            DialogScriptTurn t;
            if (td->creature_id)
                t.creature_id = td->creature_id;
            if (td->text)
                t.text = td->text;
            metadata.source_script_turns.push_back(std::move(t));
        }
    }

    return metadata;
}

std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &animationMetadata) {
    auto metadataDto = AnimationMetadataDto::createShared();
    metadataDto->animation_id = animationMetadata.animation_id;
    metadataDto->title = animationMetadata.title;
    metadataDto->milliseconds_per_frame = animationMetadata.milliseconds_per_frame;
    metadataDto->note = animationMetadata.note;
    metadataDto->sound_file = animationMetadata.sound_file;
    metadataDto->number_of_frames = animationMetadata.number_of_frames;
    metadataDto->multitrack_audio = animationMetadata.multitrack_audio;
    if (!animationMetadata.source_stage_id.empty()) {
        metadataDto->source_stage_id = animationMetadata.source_stage_id;
        metadataDto->source_stage_updated_at = animationMetadata.source_stage_updated_at;
    }
    if (animationMetadata.render_seed != 0) {
        metadataDto->render_seed = animationMetadata.render_seed;
    }
    if (!animationMetadata.source_render_choices.empty()) {
        auto choices = oatpp::List<oatpp::Object<CreatureRenderChoiceDto>>::createShared();
        for (const auto &choice : animationMetadata.source_render_choices) {
            auto choiceDto = CreatureRenderChoiceDto::createShared();
            choiceDto->creature_id = choice.creature_id;
            choiceDto->speech_loop_animation_id = choice.speech_loop_animation_id;
            choiceDto->idle_animation_id = choice.idle_animation_id;
            choiceDto->idle_start_offset = choice.idle_start_offset;
            choices->push_back(choiceDto);
        }
        metadataDto->source_render_choices = choices;
    }
    if (!animationMetadata.source_script_id.empty()) {
        metadataDto->source_script_id = animationMetadata.source_script_id;
    }
    if (!animationMetadata.source_script_turns.empty()) {
        auto turns = oatpp::List<oatpp::Object<DialogScriptTurnDto>>::createShared();
        for (const auto &t : animationMetadata.source_script_turns) {
            auto td = DialogScriptTurnDto::createShared();
            td->creature_id = t.creature_id;
            td->text = t.text;
            turns->push_back(td);
        }
        metadataDto->source_script_turns = turns;
    }

    return metadataDto.getPtr();
}

} // namespace creatures