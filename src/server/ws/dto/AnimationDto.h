#pragma once

#include <memory>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Animation.h"
#include "server/ws/dto/DialogScriptDto.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class TrackDto : public oatpp::DTO {
    DTO_INIT(TrackDto, DTO)
    DTO_FIELD(String, id);
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, fixture_id);
    DTO_FIELD(String, animation_id);
    DTO_FIELD(List<String>, frames);
};

class CreatureRenderChoiceDto final : public oatpp::DTO {
    DTO_INIT(CreatureRenderChoiceDto, DTO)
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, speech_loop_animation_id);
    DTO_FIELD(String, idle_animation_id);
    DTO_FIELD(UInt32, idle_start_offset);
};

class AnimationMetadataDto final : public oatpp::DTO {
    DTO_INIT(AnimationMetadataDto, DTO)
    DTO_FIELD(String, animation_id);
    DTO_FIELD(String, title);
    DTO_FIELD(UInt32, milliseconds_per_frame);
    DTO_FIELD(String, note);
    DTO_FIELD(String, sound_file);
    DTO_FIELD(UInt32, number_of_frames);
    DTO_FIELD(Boolean, multitrack_audio);
    DTO_FIELD(String, source_stage_id);
    DTO_FIELD(Int64, source_stage_updated_at);
    DTO_FIELD(UInt64, render_seed);
    DTO_FIELD(List<Object<CreatureRenderChoiceDto>>, source_render_choices);
    DTO_FIELD(String, source_script_id);
    DTO_FIELD(List<Object<DialogScriptTurnDto>>, source_script_turns);
};

class AnimationDto : public oatpp::DTO {
    DTO_INIT(AnimationDto, DTO)
    DTO_FIELD(String, id);
    DTO_FIELD(Object<AnimationMetadataDto>, metadata);
    DTO_FIELD(Vector<Object<TrackDto>>, tracks);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<TrackDto> convertToDto(const Track &track);
Track convertFromDto(const std::shared_ptr<TrackDto> &trackDto);
std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &metadata);
AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &metadataDto);
std::shared_ptr<AnimationDto> convertToDto(const Animation &animation);
Animation convertFromDto(const std::shared_ptr<AnimationDto> &animationDto);

} // namespace creatures
