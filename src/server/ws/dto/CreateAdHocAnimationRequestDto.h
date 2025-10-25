#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class CreateAdHocAnimationRequestDto : public oatpp::DTO {

    DTO_INIT(CreateAdHocAnimationRequestDto, DTO)

    DTO_FIELD_INFO(creature_id) {
        info->description = "Creature ID to speak";
        info->required = true;
    }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(text) {
        info->description = "Dialog for the creature";
        info->required = true;
    }
    DTO_FIELD(String, text);

    DTO_FIELD_INFO(resume_playlist) {
        info->description = "Resume interrupted playlist after speech finishes";
        info->required = false;
    }
    DTO_FIELD(Boolean, resume_playlist) = true;
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
