
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief Request body for POST /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger
 *
 * All fields are optional. With an empty body the pattern runs with its configured
 * fade-in / hold / fade-out and stays held until something else (a binding transition
 * or another trigger) stops it.
 */
class TriggerFixturePatternRequestDto : public oatpp::DTO {

    DTO_INIT(TriggerFixturePatternRequestDto, DTO)

    DTO_FIELD_INFO(stop_after_ms) {
        info->description = "If set, automatically stop the pattern this many milliseconds after triggering. Useful "
                            "for one-shot UI buttons. If unset, the pattern holds until externally stopped.";
        info->required = false;
    }
    DTO_FIELD(UInt32, stop_after_ms);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
