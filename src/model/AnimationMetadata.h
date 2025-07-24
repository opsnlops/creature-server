
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures {

struct AnimationMetadata {
    std::string animation_id;
    std::string title;
    uint32_t milliseconds_per_frame;
    std::string note;
    std::string sound_file;
    uint32_t number_of_frames;
    bool multitrack_audio;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class AnimationMetadataDto final : public oatpp::DTO {

    DTO_INIT(AnimationMetadataDto, DTO /* extends */)

    DTO_FIELD_INFO(animation_id) { info->description = "Animation ID in the form of a MongoDB OID"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(title) { info->description = "The title for this animation"; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(milliseconds_per_frame) { info->description = "The number of milliseconds per frame (usually 20)"; }
    DTO_FIELD(UInt32, milliseconds_per_frame);

    DTO_FIELD_INFO(note) { info->description = "Any notes to save in the database"; }
    DTO_FIELD(String, note);

    DTO_FIELD_INFO(sound_file) { info->description = "The sound file to play with this animation"; }
    DTO_FIELD(String, sound_file);

    DTO_FIELD_INFO(number_of_frames) { info->description = "The number of frames in the animation"; }
    DTO_FIELD(UInt32, number_of_frames);

    DTO_FIELD_INFO(multitrack_audio) { info->description = "True if the audio is multitrack"; }
    DTO_FIELD(Boolean, multitrack_audio);
};

std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &animationMetadata);
AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &animationMetadataDto);

#include OATPP_CODEGEN_END(DTO)
} // namespace creatures
