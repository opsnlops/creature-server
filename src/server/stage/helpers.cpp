
#include <cmath>
#include <string>
#include <unordered_set>

#include <spdlog/spdlog.h>

#include "model/Stage.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

namespace {

// Bounds live in model/Stage.h so every validator enforces the same caps.
// See MAX_STAGE_* / STAGE_COORD_LIMIT there.

template <typename T>
Result<T> invalidStageData(const std::shared_ptr<OperationSpan> &span, const std::string &message) {
    warn(message);
    if (span) {
        span->setError(message);
        span->setAttribute("error.type", "InvalidData");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    }
    return Result<T>{ServerError(ServerError::InvalidData, message)};
}

// Validate one coordinate axis. Rejects NaN/inf (which would poison every
// downstream bearing calculation and silently produce a garbage servo byte)
// and anything outside the +/-5 m stage cube.
std::optional<std::string> checkCoordinate(const nlohmann::json &placementJson, const char *axis, std::size_t index,
                                           float &out) {
    if (!placementJson.contains(axis) || !placementJson[axis].is_number()) {
        return fmt::format("Stage placement at index {} requires a numeric '{}'", index, axis);
    }
    const auto value = placementJson[axis].get<double>();
    if (!std::isfinite(value)) {
        return fmt::format("Stage placement at index {} has a non-finite '{}'", index, axis);
    }
    if (value < -STAGE_COORD_LIMIT || value > STAGE_COORD_LIMIT) {
        return fmt::format("Stage placement at index {} has '{}' = {}, outside the +/-{} m stage. Coordinates are "
                           "metres relative to the listener, who sits at the origin.",
                           index, axis, value, STAGE_COORD_LIMIT);
    }
    out = static_cast<float>(value);
    return std::nullopt;
}

} // namespace

Result<creatures::Stage> Database::stageFromJson(json stageJson, std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.stageFromJson, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.stageFromJson", parentSpan);

    debug("attempting to create a Stage from JSON");

    try {
        Stage stage;

        // id — required at parse time. The controller stamps it for POST
        // before calling here; the URL provides it for PUT.
        if (!stageJson.contains("id") || !stageJson["id"].is_string()) {
            return invalidStageData<Stage>(span, "Missing or invalid field 'id' in stage JSON");
        }
        stage.id = stageJson["id"].get<std::string>();
        if (stage.id.empty()) {
            return invalidStageData<Stage>(span, "Stage 'id' is empty");
        }
        if (!isUuidShape(stage.id)) {
            return invalidStageData<Stage>(span, "Stage 'id' is not a UUID");
        }

        // title — required, non-empty, bounded
        if (!stageJson.contains("title") || !stageJson["title"].is_string()) {
            return invalidStageData<Stage>(span, "Missing or invalid field 'title' in stage JSON");
        }
        stage.title = stageJson["title"].get<std::string>();
        if (stage.title.empty()) {
            return invalidStageData<Stage>(span, "Stage 'title' is empty");
        }
        if (stage.title.size() > MAX_STAGE_TITLE) {
            return invalidStageData<Stage>(
                span, fmt::format("Stage 'title' is {} chars; max {}", stage.title.size(), MAX_STAGE_TITLE));
        }

        // notes — optional, bounded
        if (stageJson.contains("notes") && !stageJson["notes"].is_null()) {
            if (!stageJson["notes"].is_string()) {
                return invalidStageData<Stage>(span, "Stage 'notes' must be a string");
            }
            stage.notes = stageJson["notes"].get<std::string>();
            if (stage.notes.size() > MAX_STAGE_NOTES) {
                return invalidStageData<Stage>(
                    span, fmt::format("Stage 'notes' is {} chars; max {}", stage.notes.size(), MAX_STAGE_NOTES));
            }
        }

        // version — optional; absent means "the current frame". Refuse to
        // parse a document from the future: if a newer client writes a v2
        // stage with a different coordinate meaning, silently treating it as
        // v1 would aim every head wrong rather than failing loudly.
        if (stageJson.contains("version") && !stageJson["version"].is_null()) {
            if (!stageJson["version"].is_number_integer()) {
                return invalidStageData<Stage>(span, "Stage 'version' must be an integer");
            }
            stage.version = stageJson["version"].get<int32_t>();
            if (stage.version < 1 || stage.version > STAGE_CURRENT_VERSION) {
                return invalidStageData<Stage>(
                    span, fmt::format("Stage 'version' {} is not supported by this server (current is {})",
                                      stage.version, STAGE_CURRENT_VERSION));
            }
        }

        // placements — required array (may be empty while authoring), bounded.
        // Per-placement we validate creature_id and the four geometry numbers
        // and nothing else; extra keys ride along untouched.
        if (!stageJson.contains("placements") || !stageJson["placements"].is_array()) {
            return invalidStageData<Stage>(span, "Missing or non-array field 'placements' in stage JSON");
        }
        if (stageJson["placements"].size() > MAX_STAGE_PLACEMENTS) {
            return invalidStageData<Stage>(span,
                                           fmt::format("Stage 'placements' has {} entries; max {} (one per creature "
                                                       "audio lane)",
                                                       stageJson["placements"].size(), MAX_STAGE_PLACEMENTS));
        }

        std::unordered_set<std::string> seenCreatureIds;
        for (std::size_t i = 0; i < stageJson["placements"].size(); ++i) {
            const auto &placementJson = stageJson["placements"][i];
            if (!placementJson.is_object()) {
                return invalidStageData<Stage>(span, fmt::format("Stage placement at index {} is not an object", i));
            }
            if (!placementJson.contains("creature_id") || !placementJson["creature_id"].is_string()) {
                return invalidStageData<Stage>(
                    span, fmt::format("Stage placement at index {} missing required string 'creature_id'", i));
            }
            const auto creatureId = placementJson["creature_id"].get<std::string>();
            if (creatureId.empty()) {
                return invalidStageData<Stage>(span,
                                               fmt::format("Stage placement at index {} has empty 'creature_id'", i));
            }
            // A creature can only be in one place at a time. Two placements
            // for the same creature would make "where is it" depend on
            // iteration order, which is a miserable bug to chase later.
            if (!seenCreatureIds.insert(creatureId).second) {
                return invalidStageData<Stage>(
                    span, fmt::format("Stage has more than one placement for creature '{}'", creatureId));
            }

            float parsed = 0.0f;
            for (const char *axis : {"x", "y", "z"}) {
                if (auto axisError = checkCoordinate(placementJson, axis, i, parsed)) {
                    return invalidStageData<Stage>(span, *axisError);
                }
            }
            // yaw is an angle, not a coordinate — any finite value is fine,
            // it just gets folded into (-180, 180] on read.
            if (!placementJson.contains("yaw") || !placementJson["yaw"].is_number()) {
                return invalidStageData<Stage>(span,
                                               fmt::format("Stage placement at index {} requires a numeric 'yaw'", i));
            }
            if (!std::isfinite(placementJson["yaw"].get<double>())) {
                return invalidStageData<Stage>(span,
                                               fmt::format("Stage placement at index {} has a non-finite 'yaw'", i));
            }
        }
        stage.placements = stageJson["placements"];

        // audio — optional, entirely console-owned. We check only that it's an
        // object so it can carry named settings; we never look inside.
        if (stageJson.contains("audio") && !stageJson["audio"].is_null()) {
            if (!stageJson["audio"].is_object()) {
                return invalidStageData<Stage>(span, "Stage 'audio' must be an object when present");
            }
            stage.audio = stageJson["audio"];
        }

        // created_at / updated_at — server-managed but stored in JSON so they
        // round-trip cleanly. Accept as int64 if present; default to 0.
        if (stageJson.contains("created_at") && stageJson["created_at"].is_number_integer()) {
            stage.created_at = stageJson["created_at"].get<int64_t>();
        }
        if (stageJson.contains("updated_at") && stageJson["updated_at"].is_number_integer()) {
            stage.updated_at = stageJson["updated_at"].get<int64_t>();
        }

        if (span) {
            span->setSuccess();
            span->setAttribute("stage.id", stage.id);
            span->setAttribute("stage.title", stage.title);
            span->setAttribute("stage.version", static_cast<int64_t>(stage.version));
            span->setAttribute("stage.placements_count", static_cast<int64_t>(stage.placements.size()));
        }
        debug("✅ Successfully created stage from JSON: id='{}', title='{}', placements={}", stage.id, stage.title,
              stage.placements.size());
        return Result<Stage>{stage};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while converting JSON to Stage: {}", e.what());
        warn(errorMessage);
        if (span) {
            span->recordException(e);
            span->setAttribute("error.type", "JsonParsingException");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<Stage>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::Stage> Database::parseStageJson(json stageJson, std::shared_ptr<OperationSpan> parentSpan) {
    return stageFromJson(std::move(stageJson), std::move(parentSpan));
}

} // namespace creatures
