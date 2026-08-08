
#include <filesystem>
#include <iterator>
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

        // stage_id (#119): optional pointer to the physical arrangement this
        // script is normally rendered against. Not validated against the
        // stages collection here — a script may legitimately outlive a stage,
        // and the render path handles a dangling id by skipping head aiming.
        if (scriptJson.contains("stage_id") && !scriptJson["stage_id"].is_null()) {
            if (!scriptJson["stage_id"].is_string()) {
                return invalidScriptData<DialogScript>(span, "Dialog script 'stage_id' must be a string");
            }
            script.stage_id = scriptJson["stage_id"].get<std::string>();
            if (!script.stage_id.empty() && !isUuidShape(script.stage_id)) {
                return invalidScriptData<DialogScript>(span, "Dialog script 'stage_id' is not a UUID");
            }
        }

        // accepted_voice (#131): the explicitly chosen take. Optional, and
        // preserved verbatim across ordinary edits — an acceptance is never
        // silently dropped, only reported stale when the turns move on.
        if (scriptJson.contains("accepted_voice") && !scriptJson["accepted_voice"].is_null()) {
            const auto &voiceJson = scriptJson["accepted_voice"];
            if (!voiceJson.is_object()) {
                return invalidScriptData<DialogScript>(span, "Dialog script 'accepted_voice' must be an object");
            }
            creatures::AcceptedVoice voice;
            voice.generation_id = voiceJson.value("generation_id", std::string{});
            voice.dialog_cache_key = voiceJson.value("dialog_cache_key", std::string{});
            voice.sound_file = voiceJson.value("sound_file", std::string{});
            if (voiceJson.contains("accepted_at") && voiceJson["accepted_at"].is_number_integer()) {
                voice.accepted_at = voiceJson["accepted_at"].get<int64_t>();
            }

            if (voice.generation_id.empty() || !isUuidShape(voice.generation_id)) {
                return invalidScriptData<DialogScript>(span,
                                                       "Dialog script 'accepted_voice.generation_id' must be a UUID");
            }
            // 64 lowercase hex — the shape computeCacheKey produces. Checked
            // because the staleness comparison is only meaningful against a
            // real key; a malformed one would read as permanently stale.
            if (voice.dialog_cache_key.size() != 64 ||
                voice.dialog_cache_key.find_first_not_of("0123456789abcdef") != std::string::npos) {
                return invalidScriptData<DialogScript>(
                    span, "Dialog script 'accepted_voice.dialog_cache_key' must be a 64-character lowercase hex "
                          "sha256");
            }
            if (voice.sound_file.size() > MAX_DIALOG_MUSIC_SOUND_FILE) {
                return invalidScriptData<DialogScript>(
                    span, "Dialog script 'accepted_voice.sound_file' is unreasonably long");
            }
            script.accepted_voice = std::move(voice);
        }

        if (scriptJson.contains("background_music") && !scriptJson["background_music"].is_null()) {
            const auto &musicJson = scriptJson["background_music"];
            if (!musicJson.is_object()) {
                return invalidScriptData<DialogScript>(span, "Dialog script 'background_music' must be an object");
            }
            for (const char *field : {"sound_file", "generation_id", "prompt"}) {
                if (!musicJson.contains(field) || !musicJson[field].is_string() ||
                    musicJson[field].get<std::string>().empty()) {
                    return invalidScriptData<DialogScript>(
                        span, fmt::format("Dialog script 'background_music.{}' must be a non-empty string", field));
                }
            }
            DialogBackgroundMusic music;
            music.sound_file = musicJson["sound_file"].get<std::string>();
            music.generation_id = musicJson["generation_id"].get<std::string>();
            music.prompt = musicJson["prompt"].get<std::string>();
            if (music.sound_file.size() > MAX_DIALOG_MUSIC_SOUND_FILE) {
                return invalidScriptData<DialogScript>(span, "Dialog script background music sound_file is too long");
            }
            const std::filesystem::path soundPath(music.sound_file);
            const auto normalPath = soundPath.lexically_normal();
            if (soundPath.is_absolute() || soundPath.has_root_path() || normalPath != soundPath ||
                soundPath.extension() != ".wav" || soundPath.begin() == soundPath.end() ||
                *soundPath.begin() != "dialog" || std::next(soundPath.begin()) == soundPath.end() ||
                *std::next(soundPath.begin()) != "music") {
                return invalidScriptData<DialogScript>(
                    span, "Dialog script background music must be a normalized WAV path under dialog/music");
            }
            if (!isUuidShape(music.generation_id)) {
                return invalidScriptData<DialogScript>(span,
                                                       "Dialog script background music generation_id must be a UUID");
            }
            if (music.prompt.size() > MAX_DIALOG_MUSIC_PROMPT) {
                return invalidScriptData<DialogScript>(span, "Dialog script background music prompt is too long");
            }
            if (!musicJson.contains("accepted_at") || !musicJson["accepted_at"].is_number_integer() ||
                musicJson["accepted_at"].get<int64_t>() <= 0) {
                return invalidScriptData<DialogScript>(
                    span, "Dialog script background music accepted_at must be a positive integer");
            }
            music.accepted_at = musicJson["accepted_at"].get<int64_t>();
            script.background_music = std::move(music);
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
