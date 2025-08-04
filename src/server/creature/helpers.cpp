

#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>

#include <bsoncxx/array/element.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/pool.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "exception/exception.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;

namespace creatures {

extern std::vector<std::string> animation_required_top_level_fields;
extern std::vector<std::string> animation_required_metadata_fields;
extern std::vector<std::string> animation_required_track_fields;

extern std::vector<std::string> creature_required_top_level_fields;
extern std::vector<std::string> creature_required_input_fields;

extern std::vector<std::string> playlist_required_fields;
extern std::vector<std::string> playlistitems_required_fields;

extern std::shared_ptr<ObservabilityManager> observability;

Result<creatures::Creature> Database::creatureFromJson(json creatureJson, std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.creatureFromJson, creating a root span");
    }

    auto span = creatures::observability->createChildOperationSpan("Database.creatureFromJson", parentSpan);

    debug("attempting to create a creature from JSON via creatureFromJson()");
    debug("JSON size: {} bytes, dump preview: {}", creatureJson.dump().length(),
          creatureJson.dump().substr(0, std::min(200UL, creatureJson.dump().length())));

    try {

        auto creature = Creature();

        // Safe JSON field access with validation
        debug("Validating 'id' field in creature JSON");
        if (!creatureJson.contains("id") || creatureJson["id"].is_null()) {
            std::string errorMessage = "Missing or null field 'id' in creature JSON";
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        creature.id = creatureJson["id"];
        debug("Successfully parsed creature id: '{}'", creature.id);

        debug("Validating 'name' field in creature JSON");
        if (!creatureJson.contains("name") || creatureJson["name"].is_null()) {
            std::string errorMessage = "Missing or null field 'name' in creature JSON";
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        creature.name = creatureJson["name"];
        debug("Successfully parsed creature name: '{}'", creature.name);

        debug("Validating 'audio_channel' field in creature JSON");
        if (!creatureJson.contains("audio_channel") || creatureJson["audio_channel"].is_null()) {
            std::string errorMessage = "Missing or null field 'audio_channel' in creature JSON";
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        creature.audio_channel = creatureJson["audio_channel"];
        debug("Successfully parsed creature audio_channel: {}", creature.audio_channel);

        debug("Validating 'channel_offset' field in creature JSON");
        if (!creatureJson.contains("channel_offset") || creatureJson["channel_offset"].is_null()) {
            std::string errorMessage = "Missing or null field 'channel_offset' in creature JSON";
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        creature.channel_offset = creatureJson["channel_offset"];
        debug("Successfully parsed creature channel_offset: {}", creature.channel_offset);

        if (creature.id.empty()) {
            std::string errorMessage = "Creature ID is empty";
            warn(errorMessage);
            span->setError(errorMessage);
            span->setAttribute("error.type", "InvalidData");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        if (creature.name.empty()) {
            std::string errorMessage = "Creature name is empty";
            warn(errorMessage);
            span->setError(errorMessage);
            span->setAttribute("error.type", "InvalidData");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }

        // Check and parse inputs
        if (creatureJson.contains("inputs")) {
            for (const auto &inputJson : creatureJson["inputs"]) {

                auto inputSpan =
                    creatures::observability->createChildOperationSpan("creatureFromJson::parseInputs", span);

                auto input = Input();
                input.slot = inputJson.value("slot", std::numeric_limits<uint16_t>::max());
                input.width = inputJson.value("width", std::numeric_limits<uint8_t>::max());
                input.name = inputJson.value("name", "-?-");
                input.joystick_axis = inputJson.value("joystick_axis", std::numeric_limits<uint8_t>::max());

                // Let's check this before going on
                if (input.slot == std::numeric_limits<uint16_t>::max()) {
                    std::string errorMessage = "Input slot is missing or invalid";
                    warn(errorMessage);
                    inputSpan->setError(errorMessage);
                    inputSpan->setAttribute("error.type", "InvalidData");
                    inputSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                if (input.width == std::numeric_limits<uint8_t>::max()) {
                    std::string errorMessage = "Input width is missing or invalid";
                    warn(errorMessage);
                    inputSpan->setError(errorMessage);
                    inputSpan->setAttribute("error.type", "InvalidData");
                    inputSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                if (input.name.empty() || input.name == "-?-") {
                    std::string errorMessage = "Input name is missing";
                    warn(errorMessage);
                    inputSpan->setError(errorMessage);
                    inputSpan->setAttribute("error.type", "InvalidData");
                    inputSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                if (input.joystick_axis == std::numeric_limits<uint8_t>::max()) {
                    std::string errorMessage = "Input joystick_axis is missing or invalid";
                    warn(errorMessage);
                    inputSpan->setError(errorMessage);
                    inputSpan->setAttribute("error.type", "InvalidData");
                    inputSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                debug("adding input: {}, slot: {}, width: {}, axis: {}", input.name, input.slot, input.width,
                      input.joystick_axis);
                creature.inputs.emplace_back(input);

                if (inputSpan) {
                    inputSpan->setSuccess();
                    inputSpan->setAttribute("input.slot", static_cast<int64_t>(input.slot));
                    inputSpan->setAttribute("input.width", static_cast<int64_t>(input.width));
                    inputSpan->setAttribute("input.name", input.name);
                    inputSpan->setAttribute("input.joystick_axis", static_cast<int64_t>(input.joystick_axis));
                }
            }
        } else {
            warn("No inputs for {} found in JSON", creature.name);
            // Don't fail, this isn't fatal
        }

        debug("✅ Successfully created creature from JSON: id='{}', name='{}', audio_channel={}, channel_offset={}, "
              "inputs_count={}",
              creature.id, creature.name, creature.audio_channel, creature.channel_offset, creature.inputs.size());
        span->setSuccess();
        span->setAttribute("creature.id", creature.id);
        span->setAttribute("creature.name", creature.name);
        span->setAttribute("creature.audio_channel", creature.audio_channel);
        span->setAttribute("creature.channel_offset", creature.channel_offset);
        span->setAttribute("creature.inputs_count", static_cast<uint32_t>(creature.inputs.size()));
        return Result<creatures::Creature>{creature};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while converting JSON to Creature: {}", e.what());
        warn(errorMessage);
        span->recordException(e);
        span->setAttribute("error.type", "JsonParsingException");
        span->setAttribute("error.message", e.what());
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<bool> Database::has_required_fields(const nlohmann::json &j, const std::vector<std::string> &required_fields) {
    for (const auto &field : required_fields) {
        if (!j.contains(field)) {
            std::string errorMessage = fmt::format("Missing required field '{}'", field);
            warn(errorMessage);
            return Result<bool>{ServerError(ServerError::InvalidData, errorMessage)};
        }
    }

    return Result<bool>{true};
}

Result<bool> Database::validateCreatureJson(const nlohmann::json &json) {

    auto topOkay = has_required_fields(json, creature_required_top_level_fields);
    if (!topOkay.isSuccess()) {
        return topOkay;
    }

    // If there's inputs, validate them
    if (json.contains("inputs")) {
        for (const auto &input : json["inputs"]) {
            auto inputOkay = has_required_fields(input, creature_required_input_fields);
            if (!inputOkay.isSuccess()) {
                return inputOkay;
            }
        }
    }

    return Result<bool>{true};
}

/*
 * This is for animations, not creatures. For some reason the linker doesn't
 * like it in the place and heck if I know.
 */

Result<bool> Database::validateAnimationJson(const nlohmann::json &json) {

    auto topLevelOkay = has_required_fields(json, creatures::animation_required_top_level_fields);
    if (!topLevelOkay.isSuccess()) {
        return topLevelOkay;
    }

    auto metadataOkay = has_required_fields(json["metadata"], animation_required_metadata_fields);
    if (!metadataOkay.isSuccess()) {
        return metadataOkay;
    }

    // Confirm that the tracks are valid
    for (const auto &track : json["tracks"]) {
        auto trackOkay = has_required_fields(track, animation_required_track_fields);
        if (!trackOkay.isSuccess()) {
            return trackOkay;
        }
    }

    // TODO: Make sure that the creature_ids in the tracks are valid

    return Result<bool>{true};
}

Result<bool> Database::validatePlaylistJson(const nlohmann::json &json) {

    auto topLevelOkay = has_required_fields(json, creatures::playlist_required_fields);
    if (!topLevelOkay.isSuccess()) {
        return topLevelOkay;
    }

    // Confirm that the items are valid
    for (const auto &item : json["items"]) {
        auto itemOkay = has_required_fields(item, playlistitems_required_fields);
        if (!itemOkay.isSuccess()) {
            return itemOkay;
        }
    }

    return Result<bool>{true};
}
} // namespace creatures