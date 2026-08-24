#pragma once

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "model/Creature.h"
#include "model/JsonCodec.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures::api {

inline constexpr std::size_t MAX_IDLE_TOGGLE_REQUEST_BODY_BYTES = 4096;
// The creature config is JSON encoded inside the registration envelope. In the
// worst case every byte needs one additional JSON escaping byte, plus a small
// fixed allowance for the envelope fields.
inline constexpr std::size_t MAX_REGISTER_CREATURE_REQUEST_BODY_BYTES = MAX_CREATURE_REQUEST_BODY_BYTES * 2 + 256;

struct IdleToggleRequest {
    bool enabled;
};

inline Result<IdleToggleRequest> idleToggleRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "idle toggle request", {"enabled"});
    if (!fields.isSuccess())
        return Result<IdleToggleRequest>{fields.getError().value()};
    const auto enabled = json.find("enabled");
    if (enabled == json.end())
        return json_codec::invalid<IdleToggleRequest>("idle toggle request.enabled is required");
    if (!enabled->is_boolean())
        return json_codec::invalid<IdleToggleRequest>("idle toggle request.enabled must be a boolean");
    return Result<IdleToggleRequest>{IdleToggleRequest{enabled->get<bool>()}};
}

struct RegisterCreatureRequest {
    std::string creatureConfig;
    universe_t universe;
};

inline Result<RegisterCreatureRequest> registerCreatureRequestFromJson(const nlohmann::json &json) {
    auto fields =
        json_codec::rejectUnknownFields(json, "creature registration request", {"creature_config", "universe"});
    if (!fields.isSuccess())
        return Result<RegisterCreatureRequest>{fields.getError().value()};

    auto creatureConfig = json_codec::requiredString(json, "creature registration request", "creature_config",
                                                     MAX_CREATURE_REQUEST_BODY_BYTES);
    if (!creatureConfig.isSuccess())
        return Result<RegisterCreatureRequest>{creatureConfig.getError().value()};

    const auto universeValue = json.find("universe");
    if (universeValue == json.end())
        return json_codec::invalid<RegisterCreatureRequest>("creature registration request.universe is required");
    if (!universeValue->is_number_unsigned() && !universeValue->is_number_integer()) {
        return json_codec::invalid<RegisterCreatureRequest>(
            "creature registration request.universe must be an integer");
    }

    uint64_t universe = 0;
    if (universeValue->is_number_unsigned()) {
        universe = universeValue->get<uint64_t>();
    } else {
        const auto signedUniverse = universeValue->get<int64_t>();
        if (signedUniverse < 0) {
            return json_codec::invalid<RegisterCreatureRequest>(
                "creature registration request.universe must be non-negative");
        }
        universe = static_cast<uint64_t>(signedUniverse);
    }
    if (universe < 1 || universe > 63999) {
        return json_codec::invalid<RegisterCreatureRequest>(
            "creature registration request.universe must be in [1, 63999]");
    }
    return Result<RegisterCreatureRequest>{
        RegisterCreatureRequest{creatureConfig.getValue().value(), static_cast<universe_t>(universe)}};
}

} // namespace creatures::api
