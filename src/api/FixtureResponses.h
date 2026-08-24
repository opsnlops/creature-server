#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace creatures::api {

struct FixtureConfigValidationResponse {
    bool valid{false};
    std::string fixtureId;
    std::vector<std::string> missingCreatureIds;
    std::vector<std::string> errorMessages;
};

inline nlohmann::json fixtureConfigValidationResponseToJson(const FixtureConfigValidationResponse &response) {
    return {{"valid", response.valid},
            {"fixture_id", response.fixtureId},
            {"missing_creature_ids", response.missingCreatureIds},
            {"error_messages", response.errorMessages}};
}

} // namespace creatures::api
