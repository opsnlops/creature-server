
#include <string>

#include <spdlog/spdlog.h>

#include "model/Storyboard.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

namespace {

// Bounds live in model/Storyboard.h so every validator (controller, DB layer,
// future callers) enforces the same caps. See MAX_STORYBOARD_* there.

template <typename T>
Result<T> invalidStoryboardData(const std::shared_ptr<OperationSpan> &span, const std::string &message) {
    warn(message);
    if (span) {
        span->setError(message);
        span->setAttribute("error.type", "InvalidData");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    }
    return Result<T>{ServerError(ServerError::InvalidData, message)};
}

} // namespace

Result<creatures::Storyboard> Database::storyboardFromJson(json storyboardJson,
                                                           std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.storyboardFromJson, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.storyboardFromJson", parentSpan);

    debug("attempting to create a Storyboard from JSON");

    try {
        Storyboard storyboard;

        // id — required at parse time. The controller stamps it for POST
        // before calling here; the URL provides it for PUT.
        if (!storyboardJson.contains("id") || !storyboardJson["id"].is_string()) {
            return invalidStoryboardData<Storyboard>(span, "Missing or invalid field 'id' in storyboard JSON");
        }
        storyboard.id = storyboardJson["id"].get<std::string>();
        if (storyboard.id.empty()) {
            return invalidStoryboardData<Storyboard>(span, "Storyboard 'id' is empty");
        }
        // Storyboard IDs are always UUIDs (server-stamped on create, URL-supplied on update).
        // Defensive check at the parser so anything that gets here without going through
        // the controller's UUID guard still gets rejected.
        if (!isUuidShape(storyboard.id)) {
            return invalidStoryboardData<Storyboard>(span, "Storyboard 'id' is not a UUID");
        }

        // title — required, non-empty, bounded
        if (!storyboardJson.contains("title") || !storyboardJson["title"].is_string()) {
            return invalidStoryboardData<Storyboard>(span, "Missing or invalid field 'title' in storyboard JSON");
        }
        storyboard.title = storyboardJson["title"].get<std::string>();
        if (storyboard.title.empty()) {
            return invalidStoryboardData<Storyboard>(span, "Storyboard 'title' is empty");
        }
        if (storyboard.title.size() > MAX_STORYBOARD_TITLE) {
            return invalidStoryboardData<Storyboard>(span, fmt::format("Storyboard 'title' is {} chars; max {}",
                                                                       storyboard.title.size(), MAX_STORYBOARD_TITLE));
        }

        // notes — optional, bounded
        if (storyboardJson.contains("notes") && !storyboardJson["notes"].is_null()) {
            if (!storyboardJson["notes"].is_string()) {
                return invalidStoryboardData<Storyboard>(span, "Storyboard 'notes' must be a string");
            }
            storyboard.notes = storyboardJson["notes"].get<std::string>();
            if (storyboard.notes.size() > MAX_STORYBOARD_NOTES) {
                return invalidStoryboardData<Storyboard>(span,
                                                         fmt::format("Storyboard 'notes' is {} chars; max {}",
                                                                     storyboard.notes.size(), MAX_STORYBOARD_NOTES));
            }
        }

        // tiles — required array (may be empty during authoring), bounded.
        // We do shallow per-tile validation (id, label length) but NEVER look
        // inside `action` — that's the forward-compat seam. The whole tiles
        // array is carried through as opaque nlohmann::json so unknown action
        // shapes round-trip losslessly.
        if (!storyboardJson.contains("tiles") || !storyboardJson["tiles"].is_array()) {
            return invalidStoryboardData<Storyboard>(span, "Missing or non-array field 'tiles' in storyboard JSON");
        }
        if (storyboardJson["tiles"].size() > MAX_STORYBOARD_TILES) {
            return invalidStoryboardData<Storyboard>(span,
                                                     fmt::format("Storyboard 'tiles' has {} entries; max {}",
                                                                 storyboardJson["tiles"].size(), MAX_STORYBOARD_TILES));
        }
        for (std::size_t i = 0; i < storyboardJson["tiles"].size(); ++i) {
            const auto &tileJson = storyboardJson["tiles"][i];
            if (!tileJson.is_object()) {
                return invalidStoryboardData<Storyboard>(
                    span, fmt::format("Storyboard tile at index {} is not an object", i));
            }
            if (!tileJson.contains("id") || !tileJson["id"].is_string()) {
                return invalidStoryboardData<Storyboard>(
                    span, fmt::format("Storyboard tile at index {} missing required string 'id'", i));
            }
            const auto tileId = tileJson["id"].get<std::string>();
            if (tileId.empty()) {
                return invalidStoryboardData<Storyboard>(span,
                                                         fmt::format("Storyboard tile at index {} has empty 'id'", i));
            }
            // Tile IDs are client-generated UUIDs per the contract — reject anything that
            // isn't UUID-shaped before it lands in Mongo (would otherwise be a silent
            // forever-bug surfaced only when a client tries to look up the tile).
            if (!isUuidShape(tileId)) {
                return invalidStoryboardData<Storyboard>(
                    span, fmt::format("Storyboard tile at index {} 'id' is not a UUID", i));
            }
            if (tileJson.contains("label") && !tileJson["label"].is_null()) {
                if (!tileJson["label"].is_string()) {
                    return invalidStoryboardData<Storyboard>(
                        span, fmt::format("Storyboard tile at index {} 'label' must be a string", i));
                }
                const auto labelLen = tileJson["label"].get<std::string>().size();
                if (labelLen > MAX_STORYBOARD_TILE_LABEL) {
                    return invalidStoryboardData<Storyboard>(
                        span, fmt::format("Storyboard tile at index {} 'label' is {} chars; max {}", i, labelLen,
                                          MAX_STORYBOARD_TILE_LABEL));
                }
            }
            // `action`: tolerate absence (a tile may be a placeholder under
            // construction). When present it MUST be an object (otherwise it
            // can't carry the client's `type` discriminator). We deliberately
            // stop here — no key introspection, no type whitelisting.
            if (tileJson.contains("action") && !tileJson["action"].is_null() && !tileJson["action"].is_object()) {
                return invalidStoryboardData<Storyboard>(
                    span, fmt::format("Storyboard tile at index {} 'action' must be an object when present", i));
            }
        }
        storyboard.tiles = storyboardJson["tiles"];

        // created_at / updated_at — server-managed but stored in JSON so they
        // round-trip cleanly. Accept as int64 if present; default to 0.
        if (storyboardJson.contains("created_at") && storyboardJson["created_at"].is_number_integer()) {
            storyboard.created_at = storyboardJson["created_at"].get<int64_t>();
        }
        if (storyboardJson.contains("updated_at") && storyboardJson["updated_at"].is_number_integer()) {
            storyboard.updated_at = storyboardJson["updated_at"].get<int64_t>();
        }

        if (span) {
            span->setSuccess();
            span->setAttribute("storyboard.id", storyboard.id);
            span->setAttribute("storyboard.title", storyboard.title);
            span->setAttribute("storyboard.tiles_count", static_cast<int64_t>(storyboard.tiles.size()));
        }
        debug("✅ Successfully created storyboard from JSON: id='{}', title='{}', tiles={}", storyboard.id,
              storyboard.title, storyboard.tiles.size());
        return Result<Storyboard>{storyboard};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while converting JSON to Storyboard: {}", e.what());
        warn(errorMessage);
        if (span) {
            span->recordException(e);
            span->setAttribute("error.type", "JsonParsingException");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<Storyboard>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::Storyboard> Database::parseStoryboardJson(json storyboardJson,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    return storyboardFromJson(std::move(storyboardJson), std::move(parentSpan));
}

} // namespace creatures
