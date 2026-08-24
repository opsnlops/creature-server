#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/Creature.h"
#include "server/runtime/RuntimeSnapshot.h"

namespace creatures::api {

/// Framework-neutral REST representation of a creature. Configuration and
/// runtime state deliberately remain separate in memory, then join only at the
/// API boundary.
struct CreatureResponse {
    Creature creature;
    runtime::CreatureRuntimeSnapshot runtime;
};

inline nlohmann::json creatureResponseToJson(const CreatureResponse &response) {
    auto json = creatureToJson(response.creature);
    json["runtime"] = runtime::creatureRuntimeSnapshotToJson(response.runtime);
    return json;
}

struct CreatureConfigValidationResponse {
    bool valid{true};
    std::optional<std::string> creatureId;
    std::vector<std::string> missingAnimationIds;
    std::vector<std::string> mismatchedAnimationIds;
    std::vector<std::string> errorMessages;
};

inline nlohmann::json creatureConfigValidationResponseToJson(const CreatureConfigValidationResponse &response) {
    return {{"valid", response.valid},
            {"creature_id", response.creatureId ? nlohmann::json(*response.creatureId) : nlohmann::json(nullptr)},
            {"missing_animation_ids", response.missingAnimationIds},
            {"mismatched_animation_ids", response.mismatchedAnimationIds},
            {"error_messages", response.errorMessages}};
}

} // namespace creatures::api
