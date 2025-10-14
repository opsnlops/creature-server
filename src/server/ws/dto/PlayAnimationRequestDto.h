
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures ::ws {

class PlayAnimationRequestDto : public oatpp::DTO {

    DTO_INIT(PlayAnimationRequestDto, DTO)

    DTO_FIELD_INFO(animation_id) {
        info->description = "The animation to play";
        info->required = true;
    }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(universe) {
        info->description = "Which universe to play the animation in";
        info->required = true;
    }
    DTO_FIELD(UInt32, universe);

    DTO_FIELD_INFO(resumePlaylist) {
        info->description = "Whether to resume the playlist after the interrupt animation completes";
        info->required = false;
    }
    DTO_FIELD(Boolean, resumePlaylist) = false;
};
} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)
