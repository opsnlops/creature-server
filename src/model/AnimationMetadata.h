
#pragma once

#include <vector>
#include <string>


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

    class AnimationMetadataDto : public oatpp::DTO {

        DTO_INIT(AnimationMetadataDto, DTO /* extends */)

        DTO_FIELD(String, animation_id);
        DTO_FIELD(String, title);
        DTO_FIELD(UInt32, milliseconds_per_frame);
        DTO_FIELD(String, note);
        DTO_FIELD(String, sound_file);
        DTO_FIELD(UInt32, number_of_frames);
        DTO_FIELD(Boolean, multitrack_audio);

    };


    std::shared_ptr<AnimationMetadataDto> convertToDto(const AnimationMetadata &animationMetadata);
    AnimationMetadata convertFromDto(const std::shared_ptr<AnimationMetadataDto> &animationMetadataDto);

#include OATPP_CODEGEN_END(DTO)
}
