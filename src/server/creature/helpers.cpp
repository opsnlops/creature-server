
#include <algorithm>
#include <optional>
#include <string>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <mongocxx/pool.hpp>

#include <bsoncxx/builder/stream/document.hpp>

#include "exception/exception.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

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

        debug("Validating 'mouth_slot' field in creature JSON");
        if (!creatureJson.contains("mouth_slot") || creatureJson["mouth_slot"].is_null()) {
            std::string errorMessage = "Missing or null field 'mouth_slot' in creature JSON";
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        creature.mouth_slot = creatureJson["mouth_slot"];
        debug("Successfully parsed creature mouth_slot: {}", static_cast<int>(creature.mouth_slot));

        if (creatureJson.contains("speech_loop_animation_ids") &&
            !creatureJson["speech_loop_animation_ids"].is_null()) {
            if (!creatureJson["speech_loop_animation_ids"].is_array()) {
                std::string errorMessage = "'speech_loop_animation_ids' must be an array of animation IDs";
                warn(errorMessage);
                span->setError(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            for (const auto &value : creatureJson["speech_loop_animation_ids"]) {
                if (!value.is_string()) {
                    std::string errorMessage = "All 'speech_loop_animation_ids' entries must be strings";
                    warn(errorMessage);
                    span->setError(errorMessage);
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                const std::string animationId = value.get<std::string>();
                if (animationId.empty()) {
                    std::string errorMessage = "Speech loop animation IDs cannot be empty";
                    warn(errorMessage);
                    span->setError(errorMessage);
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                creature.speech_loop_animation_ids.emplace_back(animationId);
            }

            debug("Parsed {} speech loop animation IDs", creature.speech_loop_animation_ids.size());
        }

        if (creatureJson.contains("idle_animation_ids") && !creatureJson["idle_animation_ids"].is_null()) {
            if (!creatureJson["idle_animation_ids"].is_array()) {
                std::string errorMessage = "'idle_animation_ids' must be an array of animation IDs";
                warn(errorMessage);
                span->setError(errorMessage);
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            for (const auto &value : creatureJson["idle_animation_ids"]) {
                if (!value.is_string()) {
                    std::string errorMessage = "All 'idle_animation_ids' entries must be strings";
                    warn(errorMessage);
                    span->setError(errorMessage);
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }

                const std::string animationId = value.get<std::string>();
                if (animationId.empty()) {
                    std::string errorMessage = "Idle animation IDs cannot be empty";
                    warn(errorMessage);
                    span->setError(errorMessage);
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
                }
                creature.idle_animation_ids.emplace_back(animationId);
            }

            debug("Parsed {} idle animation IDs", creature.idle_animation_ids.size());
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

        // mouth_input (#120): the degree-of-freedom style reference to the
        // axis lip-sync drives. Optional; when absent the raw mouth_slot is
        // used, so existing configs are unaffected.
        if (creatureJson.contains("mouth_input") && !creatureJson["mouth_input"].is_null()) {
            if (!creatureJson["mouth_input"].is_string()) {
                std::string errorMessage = "'mouth_input' must be a string naming one of this creature's inputs";
                warn(errorMessage);
                span->setError(errorMessage);
                span->setAttribute("error.type", "InvalidData");
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            creature.mouth_input = creatureJson["mouth_input"].get<std::string>();
            if (!creature.mouth_input.empty() &&
                !creatures::inputSlotByName(creature, creature.mouth_input).has_value()) {
                std::string errorMessage = fmt::format(
                    "'mouth_input' names '{}', which is not one of this creature's inputs", creature.mouth_input);
                warn(errorMessage);
                span->setError(errorMessage);
                span->setAttribute("error.type", "InvalidData");
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }
        }

        // Gaze axes (#119). Optional, and they MUST stay optional: the
        // controller's JSON file is the source of truth and is only re-read at
        // registration, so making this required would break every existing
        // controller at boot.
        //
        // Each axis names an entry in `inputs` rather than repeating its slot,
        // because input layouts differ between creature families. We verify
        // the name resolves here, at parse time, so a typo is a 400 on upload
        // rather than a silently-dead axis discovered on stage.
        if (creatureJson.contains("gaze") && !creatureJson["gaze"].is_null()) {
            const auto &gazeJson = creatureJson["gaze"];
            if (!gazeJson.is_object()) {
                std::string errorMessage = "'gaze' must be an object";
                warn(errorMessage);
                span->setError(errorMessage);
                span->setAttribute("error.type", "InvalidData");
                return Result<creatures::Creature>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            auto parseGazeAxis = [&](const char *axisName,
                                     std::optional<creatures::GazeAxis> &target) -> std::optional<std::string> {
                if (!gazeJson.contains(axisName) || gazeJson[axisName].is_null()) {
                    return std::nullopt;
                }
                const auto &axisJson = gazeJson[axisName];
                if (!axisJson.is_object()) {
                    return fmt::format("'gaze.{}' must be an object", axisName);
                }
                if (!axisJson.contains("input") || !axisJson["input"].is_string()) {
                    return fmt::format("'gaze.{}' requires a string 'input' naming one of this creature's inputs",
                                       axisName);
                }
                creatures::GazeAxis axis{};
                axis.input = axisJson["input"].get<std::string>();
                if (axis.input.empty()) {
                    return fmt::format("'gaze.{}.input' is empty", axisName);
                }

                // The whole point of naming the input instead of numbering a
                // slot: catch the mismatch now, while we can say what's wrong.
                const auto matchingInput =
                    std::find_if(creature.inputs.begin(), creature.inputs.end(),
                                 [&](const creatures::Input &i) { return i.name == axis.input; });
                if (matchingInput == creature.inputs.end()) {
                    std::string known;
                    for (const auto &i : creature.inputs) {
                        known += known.empty() ? i.name : ", " + i.name;
                    }
                    return fmt::format("'gaze.{}.input' names '{}', which is not one of this creature's inputs ({})",
                                       axisName, axis.input, known.empty() ? "none defined" : known);
                }

                for (const char *required : {"degrees_at_min", "degrees_at_max"}) {
                    if (!axisJson.contains(required) || !axisJson[required].is_number()) {
                        return fmt::format("'gaze.{}' requires a numeric '{}'", axisName, required);
                    }
                }
                axis.degrees_at_min = axisJson["degrees_at_min"].get<float>();
                axis.degrees_at_max = axisJson["degrees_at_max"].get<float>();
                if (axis.degrees_at_min == axis.degrees_at_max) {
                    return fmt::format("'gaze.{}' has degrees_at_min == degrees_at_max (the axis has no range)",
                                       axisName);
                }

                if (axisJson.contains("listening_amount") && !axisJson["listening_amount"].is_null()) {
                    if (!axisJson["listening_amount"].is_number()) {
                        return fmt::format("'gaze.{}.listening_amount' must be a number", axisName);
                    }
                    axis.listening_amount = axisJson["listening_amount"].get<float>();
                    if (axis.listening_amount < 0.0f || axis.listening_amount > 1.0f) {
                        return fmt::format("'gaze.{}.listening_amount' must be between 0 and 1", axisName);
                    }
                }

                target = axis;
                return std::nullopt;
            };

            creatures::GazeConfig gaze;
            for (const auto &[axisName, target] :
                 std::initializer_list<std::pair<const char *, std::optional<creatures::GazeAxis> *>>{
                     {"pan", &gaze.pan}, {"elevation", &gaze.elevation}, {"cock", &gaze.cock}}) {
                if (auto axisError = parseGazeAxis(axisName, *target)) {
                    warn(*axisError);
                    span->setError(*axisError);
                    span->setAttribute("error.type", "InvalidData");
                    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, *axisError)};
                }
            }
            if (gaze.pan || gaze.elevation || gaze.cock) {
                creature.gaze = gaze;
                debug("Gaze axes: pan={}, elevation={}, cock={}", gaze.pan.has_value(), gaze.elevation.has_value(),
                      gaze.cock.has_value());
            }
        }

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

        debug("✅ Successfully created creature from JSON: id='{}', name='{}', audio_channel={}, channel_offset={}, "
              "mouth_slot={}, inputs_count={}",
              creature.id, creature.name, creature.audio_channel, creature.channel_offset,
              static_cast<int>(creature.mouth_slot), creature.inputs.size());
        span->setSuccess();
        span->setAttribute("creature.id", creature.id);
        span->setAttribute("creature.name", creature.name);
        span->setAttribute("creature.audio_channel", creature.audio_channel);
        span->setAttribute("creature.channel_offset", creature.channel_offset);
        span->setAttribute("creature.mouth_slot", static_cast<int64_t>(creature.mouth_slot));
        span->setAttribute("creature.speech_loop_animation_ids",
                           static_cast<int64_t>(creature.speech_loop_animation_ids.size()));
        span->setAttribute("creature.idle_animation_ids", static_cast<int64_t>(creature.idle_animation_ids.size()));
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

Result<creatures::Creature> Database::parseCreatureJson(json creatureJson, std::shared_ptr<OperationSpan> parentSpan) {
    return creatureFromJson(std::move(creatureJson), std::move(parentSpan));
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
