#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/// Response for POST /api/v1/animation/dialog/script/validate.
/// Shape-only check that returns 200 in both success + failure cases so the
/// client can render inline form errors without exception handling. Mirrors
/// FixtureConfigValidationDto.
class DialogScriptValidationDto : public oatpp::DTO {

    DTO_INIT(DialogScriptValidationDto, DTO)

    DTO_FIELD_INFO(valid) { info->description = "Whether the dialog script passed shape validation."; }
    DTO_FIELD(Boolean, valid);

    DTO_FIELD_INFO(script_id) {
        info->description = "Script UUID from the submitted JSON if one was present and the doc was parseable. "
                            "Empty for create-flow validation (where the server would generate one).";
    }
    DTO_FIELD(String, script_id);

    DTO_FIELD_INFO(turn_count) {
        info->description = "Number of turns in the submitted script (whether or not validation passed).";
    }
    DTO_FIELD(UInt32, turn_count);

    DTO_FIELD_INFO(missing_creature_ids) {
        info->description = "creature_ids referenced by turns that don't currently exist on the server. Soft "
                            "warning — render-time will still reject these, but it's a friendlier check at edit time.";
    }
    DTO_FIELD(List<String>, missing_creature_ids);

    DTO_FIELD_INFO(error_messages) { info->description = "Hard validation errors. Empty when valid=true."; }
    DTO_FIELD(List<String>, error_messages);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
