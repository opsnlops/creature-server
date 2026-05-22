#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class FixtureConfigValidationDto : public oatpp::DTO {
    DTO_INIT(FixtureConfigValidationDto, DTO)

    DTO_FIELD_INFO(valid) { info->description = "Whether the fixture config passed validation"; }
    DTO_FIELD(Boolean, valid);

    DTO_FIELD_INFO(fixture_id) { info->description = "Fixture UUID from the submitted config (if parseable)"; }
    DTO_FIELD(String, fixture_id);

    DTO_FIELD_INFO(missing_creature_ids) {
        info->description = "Binding creature_ids that don't currently exist on the server (soft warning, not a hard "
                            "error)";
    }
    DTO_FIELD(List<String>, missing_creature_ids);

    DTO_FIELD_INFO(error_messages) { info->description = "Validation errors"; }
    DTO_FIELD(List<String>, error_messages);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
