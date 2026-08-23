#include "AnimationDto.h"

namespace creatures {

oatpp::Object<TrackDto> convertToDto(const Track &track) {
    auto dto = TrackDto::createShared();
    dto->id = track.id;
    if (!track.creature_id.empty())
        dto->creature_id = track.creature_id;
    if (!track.fixture_id.empty())
        dto->fixture_id = track.fixture_id;
    dto->animation_id = track.animation_id;
    dto->frames = oatpp::List<oatpp::String>::createShared();
    for (const auto &frame : track.frames)
        dto->frames->push_back(frame);
    return dto;
}

Track convertFromDto(const std::shared_ptr<TrackDto> &dto) {
    Track track;
    track.id = dto->id;
    if (dto->creature_id)
        track.creature_id = dto->creature_id;
    if (dto->fixture_id)
        track.fixture_id = dto->fixture_id;
    track.animation_id = dto->animation_id;
    if (dto->frames) {
        for (const auto &frame : *dto->frames)
            track.frames.emplace_back(frame);
    }
    return track;
}

std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &metadata) {
    auto dto = AnimationMetadataDto::createShared();
    dto->animation_id = metadata.animation_id;
    dto->title = metadata.title;
    dto->milliseconds_per_frame = metadata.milliseconds_per_frame;
    dto->note = metadata.note;
    dto->sound_file = metadata.sound_file;
    dto->number_of_frames = metadata.number_of_frames;
    dto->multitrack_audio = metadata.multitrack_audio;
    if (!metadata.source_stage_id.empty()) {
        dto->source_stage_id = metadata.source_stage_id;
        dto->source_stage_updated_at = metadata.source_stage_updated_at;
    }
    if (metadata.render_seed != 0)
        dto->render_seed = metadata.render_seed;
    if (!metadata.source_render_choices.empty()) {
        dto->source_render_choices = oatpp::List<oatpp::Object<CreatureRenderChoiceDto>>::createShared();
        for (const auto &choice : metadata.source_render_choices) {
            auto choiceDto = CreatureRenderChoiceDto::createShared();
            choiceDto->creature_id = choice.creature_id;
            choiceDto->speech_loop_animation_id = choice.speech_loop_animation_id;
            if (!choice.idle_animation_id.empty())
                choiceDto->idle_animation_id = choice.idle_animation_id;
            if (choice.idle_start_offset != 0)
                choiceDto->idle_start_offset = choice.idle_start_offset;
            dto->source_render_choices->push_back(choiceDto);
        }
    }
    if (!metadata.source_script_id.empty())
        dto->source_script_id = metadata.source_script_id;
    if (!metadata.source_script_turns.empty()) {
        dto->source_script_turns = oatpp::List<oatpp::Object<DialogScriptTurnDto>>::createShared();
        for (const auto &turn : metadata.source_script_turns) {
            auto turnDto = DialogScriptTurnDto::createShared();
            turnDto->creature_id = turn.creature_id;
            turnDto->text = turn.text;
            dto->source_script_turns->push_back(turnDto);
        }
    }
    return dto.getPtr();
}

AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &dto) {
    AnimationMetadata metadata;
    metadata.animation_id = dto->animation_id;
    metadata.title = dto->title;
    metadata.milliseconds_per_frame = dto->milliseconds_per_frame;
    metadata.note = dto->note;
    metadata.sound_file = dto->sound_file;
    metadata.number_of_frames = dto->number_of_frames;
    metadata.multitrack_audio = dto->multitrack_audio;
    if (dto->source_stage_id)
        metadata.source_stage_id = dto->source_stage_id;
    if (dto->source_stage_updated_at)
        metadata.source_stage_updated_at = *dto->source_stage_updated_at;
    if (dto->render_seed)
        metadata.render_seed = *dto->render_seed;
    if (dto->source_render_choices) {
        for (const auto &choiceDto : *dto->source_render_choices) {
            if (!choiceDto)
                continue;
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
    if (dto->source_script_id)
        metadata.source_script_id = dto->source_script_id;
    if (dto->source_script_turns) {
        for (const auto &turnDto : *dto->source_script_turns) {
            if (!turnDto)
                continue;
            DialogScriptTurn turn;
            if (turnDto->creature_id)
                turn.creature_id = turnDto->creature_id;
            if (turnDto->text)
                turn.text = turnDto->text;
            metadata.source_script_turns.push_back(std::move(turn));
        }
    }
    return metadata;
}

std::shared_ptr<AnimationDto> convertToDto(const Animation &animation) {
    auto dto = AnimationDto::createShared();
    dto->id = animation.id;
    dto->metadata = convertToDto(animation.metadata);
    dto->tracks = oatpp::Vector<oatpp::Object<TrackDto>>::createShared();
    for (const auto &track : animation.tracks)
        dto->tracks->push_back(convertToDto(track));
    return dto.getPtr();
}

Animation convertFromDto(const std::shared_ptr<AnimationDto> &dto) {
    Animation animation;
    animation.id = dto->id;
    animation.metadata = convertFromDto(dto->metadata.getPtr());
    if (dto->tracks) {
        for (const auto &trackDto : *dto->tracks)
            animation.tracks.push_back(convertFromDto(trackDto.getPtr()));
    }
    return animation;
}

} // namespace creatures
