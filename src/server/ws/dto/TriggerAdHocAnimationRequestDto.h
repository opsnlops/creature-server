#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class TriggerAdHocAnimationRequestDto : public oatpp::DTO {

    DTO_INIT(TriggerAdHocAnimationRequestDto, DTO)

    DTO_FIELD_INFO(animation_id) {
        info->description = "Ad-hoc animation ID to play";
        info->required = true;
    }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(resume_playlist) {
        info->description = "Resume interrupted playlist after playback";
        info->required = false;
    }
    DTO_FIELD(Boolean, resume_playlist) = true;
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
