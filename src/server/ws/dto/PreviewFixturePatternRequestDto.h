
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/ws/dto/SetFixtureLiveRequestDto.h" // reuses FixtureLiveValueDto for the {channel, value} shape

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief Request body for POST /api/v1/fixture/{fixtureId}/pattern/preview
 *
 * Fire a one-shot pattern that is NOT persisted — the body is the entire pattern. Used by
 * the Creature Console pattern editor so "Fire" can play whatever the user has on screen
 * without first saving the fixture. Same runner path as the regular trigger; same fade-in /
 * hold / fade-out semantics; same live-control precedence (refused while live is active).
 *
 * Reuses `FixtureLiveValueDto` for the `values[]` shape since it's identical to the live
 * control request.
 */
class PreviewFixturePatternRequestDto : public oatpp::DTO {

    DTO_INIT(PreviewFixturePatternRequestDto, DTO)

    DTO_FIELD_INFO(values) {
        info->description = "Target channel values. Each `channel` must match a FixtureChannel.name on the fixture. "
                            "Channels not listed are not driven by this pattern.";
        info->required = true;
    }
    DTO_FIELD(List<Object<FixtureLiveValueDto>>, values);

    DTO_FIELD_INFO(fade_in_ms) {
        info->description = "Milliseconds to ramp from current channel values to the targets. 0 = snap.";
        info->required = false;
    }
    DTO_FIELD(UInt32, fade_in_ms);

    DTO_FIELD_INFO(fade_out_ms) {
        info->description = "Milliseconds to ramp back to pre-pattern values when the pattern stops. 0 = snap.";
        info->required = false;
    }
    DTO_FIELD(UInt32, fade_out_ms);

    DTO_FIELD_INFO(hold_ms) {
        info->description = "Milliseconds to hold the target values after fade-in. 0 = hold indefinitely until "
                            "stopped (either by stop_after_ms here, or by another pattern firing on this fixture).";
        info->required = false;
    }
    DTO_FIELD(UInt32, hold_ms);

    DTO_FIELD_INFO(stop_after_ms) {
        info->description = "If set, schedule an auto-stop this many milliseconds after the trigger. Must be in "
                            "(0, 600000]. Omit for fire-and-hold (caller is responsible for stopping).";
        info->required = false;
    }
    DTO_FIELD(UInt32, stop_after_ms);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
