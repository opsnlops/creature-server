
#include <set>
#include <string>

#include <spdlog/spdlog.h>

#include "model/DialogScript.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

namespace {

// Bounds live in model/DialogScript.h so every validator (controller, DB layer,
// JobWorker, Animation snapshot parser) enforces the same caps. See
// MAX_DIALOG_SCRIPT_* there.

template <typename T>
Result<T> invalidScriptData(const std::shared_ptr<OperationSpan> &span, const std::string &message) {
    warn(message);
    if (span) {
        span->setError(message);
        span->setAttribute("error.type", "InvalidData");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    }
    return Result<T>{ServerError(ServerError::InvalidData, message)};
}

} // namespace

Result<creatures::DialogScript> Database::dialogScriptFromJson(json scriptJson,
                                                               std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        warn("no parent span provided for Database.dialogScriptFromJson, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.dialogScriptFromJson", parentSpan);

    debug("attempting to create a DialogScript from JSON");

    try {
        DialogScript script;

        // id (required at parse time; the controller stamps it for POST before
        // calling here, and the URL provides it for PUT).
        if (!scriptJson.contains("id") || !scriptJson["id"].is_string()) {
            return invalidScriptData<DialogScript>(span, "Missing or invalid field 'id' in dialog script JSON");
        }
        script.id = scriptJson["id"].get<std::string>();
        if (script.id.empty()) {
            return invalidScriptData<DialogScript>(span, "Dialog script 'id' is empty");
        }

        // title (required, non-empty, bounded)
        if (!scriptJson.contains("title") || !scriptJson["title"].is_string()) {
            return invalidScriptData<DialogScript>(span, "Missing or invalid field 'title' in dialog script JSON");
        }
        script.title = scriptJson["title"].get<std::string>();
        if (script.title.empty()) {
            return invalidScriptData<DialogScript>(span, "Dialog script 'title' is empty");
        }
        if (script.title.size() > MAX_DIALOG_SCRIPT_TITLE) {
            return invalidScriptData<DialogScript>(span, fmt::format("Dialog script 'title' is {} chars; max {}",
                                                                     script.title.size(), MAX_DIALOG_SCRIPT_TITLE));
        }

        // notes (optional, bounded)
        if (scriptJson.contains("notes") && !scriptJson["notes"].is_null()) {
            if (!scriptJson["notes"].is_string()) {
                return invalidScriptData<DialogScript>(span, "Dialog script 'notes' must be a string");
            }
            script.notes = scriptJson["notes"].get<std::string>();
            if (script.notes.size() > MAX_DIALOG_SCRIPT_NOTES) {
                return invalidScriptData<DialogScript>(span, fmt::format("Dialog script 'notes' is {} chars; max {}",
                                                                         script.notes.size(), MAX_DIALOG_SCRIPT_NOTES));
            }
        }

        // turns (required, non-empty, bounded)
        if (!scriptJson.contains("turns") || !scriptJson["turns"].is_array()) {
            return invalidScriptData<DialogScript>(span, "Missing or non-array field 'turns' in dialog script JSON");
        }
        if (scriptJson["turns"].empty()) {
            return invalidScriptData<DialogScript>(span, "Dialog script 'turns' must be non-empty");
        }
        if (scriptJson["turns"].size() > MAX_DIALOG_SCRIPT_TURNS) {
            return invalidScriptData<DialogScript>(span,
                                                   fmt::format("Dialog script 'turns' has {} entries; max {}",
                                                               scriptJson["turns"].size(), MAX_DIALOG_SCRIPT_TURNS));
        }
        for (const auto &turnJson : scriptJson["turns"]) {
            DialogScriptTurn turn;
            if (!turnJson.contains("creature_id") || !turnJson["creature_id"].is_string()) {
                return invalidScriptData<DialogScript>(span, "Turn missing required field 'creature_id' (string)");
            }
            turn.creature_id = turnJson["creature_id"].get<std::string>();
            if (turn.creature_id.empty()) {
                return invalidScriptData<DialogScript>(span, "Turn 'creature_id' is empty");
            }
            if (!turnJson.contains("text") || !turnJson["text"].is_string()) {
                return invalidScriptData<DialogScript>(span, "Turn missing required field 'text' (string)");
            }
            turn.text = turnJson["text"].get<std::string>();
            if (turn.text.empty()) {
                return invalidScriptData<DialogScript>(span, "Turn 'text' is empty");
            }
            if (turn.text.size() > MAX_DIALOG_SCRIPT_TURN_TEXT) {
                return invalidScriptData<DialogScript>(
                    span,
                    fmt::format("Turn 'text' is {} chars; max {}", turn.text.size(), MAX_DIALOG_SCRIPT_TURN_TEXT));
            }
            script.turns.push_back(std::move(turn));
        }

        // created_at / updated_at — server-managed but stored in JSON so they
        // round-trip cleanly. Accept as int64 if present; default to 0.
        if (scriptJson.contains("created_at") && scriptJson["created_at"].is_number_integer()) {
            script.created_at = scriptJson["created_at"].get<int64_t>();
        }
        if (scriptJson.contains("updated_at") && scriptJson["updated_at"].is_number_integer()) {
            script.updated_at = scriptJson["updated_at"].get<int64_t>();
        }

        if (span) {
            span->setSuccess();
            span->setAttribute("script.id", script.id);
            span->setAttribute("script.title", script.title);
            span->setAttribute("script.turns_count", static_cast<int64_t>(script.turns.size()));
        }
        debug("✅ Successfully created dialog script from JSON: id='{}', title='{}', turns={}", script.id, script.title,
              script.turns.size());
        return Result<DialogScript>{script};

    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("Error while converting JSON to DialogScript: {}", e.what());
        warn(errorMessage);
        if (span) {
            span->recordException(e);
            span->setAttribute("error.type", "JsonParsingException");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<DialogScript>{ServerError(ServerError::InvalidData, errorMessage)};
    }
}

Result<creatures::DialogScript> Database::parseDialogScriptJson(json scriptJson,
                                                                std::shared_ptr<OperationSpan> parentSpan) {
    return dialogScriptFromJson(std::move(scriptJson), std::move(parentSpan));
}

} // namespace creatures
