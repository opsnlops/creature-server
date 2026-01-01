#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class CreatureConfigValidationDto : public oatpp::DTO {
    DTO_INIT(CreatureConfigValidationDto, DTO)

    DTO_FIELD_INFO(valid) { info->description = "Whether the creature config passed validation"; }
    DTO_FIELD(Boolean, valid);

    DTO_FIELD_INFO(creature_id) { info->description = "Creature ID from the submitted config"; }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(missing_animation_ids) { info->description = "Animation IDs not found in the database"; }
    DTO_FIELD(List<String>, missing_animation_ids);

    DTO_FIELD_INFO(mismatched_animation_ids) {
        info->description = "Animation IDs that contain tracks for other creatures";
    }
    DTO_FIELD(List<String>, mismatched_animation_ids);

    DTO_FIELD_INFO(error_messages) { info->description = "Validation errors that are not tied to an animation ID"; }
    DTO_FIELD(List<String>, error_messages);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
