#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace creatures::api {

struct FixtureConfigValidationResponse {
    bool valid{false};
    std::optional<std::string> fixtureId;
    std::vector<std::string> missingCreatureIds;
    std::vector<std::string> errorMessages;
};

inline nlohmann::json fixtureConfigValidationResponseToJson(const FixtureConfigValidationResponse &response) {
    nlohmann::json json = {{"valid", response.valid},
                           {"missing_creature_ids", response.missingCreatureIds},
                           {"error_messages", response.errorMessages}};
    if (response.fixtureId) {
        json["fixture_id"] = *response.fixtureId;
    }
    return json;
}

} // namespace creatures::api
