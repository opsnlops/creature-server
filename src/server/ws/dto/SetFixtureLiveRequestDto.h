
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief One channel-value pair inside a live-control request.
 *
 * Mirrors the shape of FixturePatternValueDto so the Swift client can reuse types.
 */
class FixtureLiveValueDto : public oatpp::DTO {

    DTO_INIT(FixtureLiveValueDto, DTO)

    DTO_FIELD_INFO(channel) {
        info->description = "Channel name (must match a FixtureChannel.name on the target fixture).";
        info->required = true;
    }
    DTO_FIELD(String, channel);

    DTO_FIELD_INFO(value) {
        info->description = "DMX value in [0, 255].";
        info->required = true;
    }
    DTO_FIELD(UInt8, value);
};

/**
 * @brief Request body for POST /api/v1/fixture/{fixtureId}/live
 *
 * Drive a fixture's channels directly with raw DMX values from a slider UI. The server
 * holds the values until `timeout_ms` elapses, then blacks them out. Live control wins
 * over patterns: the active pattern (if any) is cancelled immediately, and new patterns
 * are refused until the live session expires.
 */
class SetFixtureLiveRequestDto : public oatpp::DTO {

    DTO_INIT(SetFixtureLiveRequestDto, DTO)

    DTO_FIELD_INFO(values) {
        info->description = "Per-channel values to apply. Channels not named here retain their previous live value "
                            "(or 0 if this is the first call in a live session).";
        info->required = true;
    }
    DTO_FIELD(List<Object<FixtureLiveValueDto>>, values);

    DTO_FIELD_INFO(timeout_ms) {
        info->description = "Auto-blackout deadline in milliseconds from now. Required. Must be in (0, 600000]. "
                            "The server will black out all live channels on this fixture after this elapses, "
                            "guarding against disconnected clients leaving lights stuck on.";
        info->required = true;
    }
    DTO_FIELD(UInt32, timeout_ms);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
