#include "ScriptCacheKey.h"

#include <fmt/format.h>

#include "server/database.h"
#include "server/namespace-stuffs.h"
#include "server/voice/DialogCache.h"
#include "server/voice/DialogClient.h"

namespace creatures {
extern std::shared_ptr<Database> db;
}

namespace creatures::voice {

Result<std::string> computeScriptCacheKey(const std::vector<creatures::DialogScriptTurn> &turns,
                                          std::shared_ptr<OperationSpan> parentSpan) {
    if (turns.empty()) {
        return Result<std::string>{ServerError(ServerError::InvalidData, "script has no turns")};
    }
    if (!creatures::db) {
        return Result<std::string>{ServerError(ServerError::InternalError, "database unavailable")};
    }

    // Cache the lookup: a scene is usually two or three creatures over many
    // turns, so this is the difference between 3 reads and 30.
    std::unordered_map<std::string, std::string> voiceByCreature;
    std::vector<DialogInput> inputs;
    inputs.reserve(turns.size());

    for (const auto &turn : turns) {
        auto it = voiceByCreature.find(turn.creature_id);
        if (it == voiceByCreature.end()) {
            auto jsonResult = creatures::db->getCreatureJson(turn.creature_id, parentSpan);
            if (!jsonResult.isSuccess()) {
                return Result<std::string>{
                    ServerError(ServerError::InvalidData,
                                fmt::format("turn names creature '{}', which could not be loaded", turn.creature_id))};
            }
            const auto cj = jsonResult.getValue().value();
            if (!cj.contains("voice") || !cj["voice"].is_object() || !cj["voice"].contains("voice_id") ||
                !cj["voice"]["voice_id"].is_string()) {
                return Result<std::string>{
                    ServerError(ServerError::InvalidData,
                                fmt::format("creature '{}' has no voice.voice_id configured", turn.creature_id))};
            }
            it = voiceByCreature.emplace(turn.creature_id, cj["voice"]["voice_id"].get<std::string>()).first;
        }
        inputs.push_back(DialogInput{it->second, turn.text});
    }

    return Result<std::string>{computeCacheKey(inputs)};
}

std::optional<bool> acceptedVoiceIsFresh(const creatures::DialogScript &script,
                                         std::shared_ptr<OperationSpan> parentSpan) {
    if (!script.accepted_voice) {
        return false;
    }
    auto key = computeScriptCacheKey(script.turns, std::move(parentSpan));
    if (!key.isSuccess()) {
        return std::nullopt; // couldn't tell — caller decides how to report it
    }
    return key.getValue().value() == script.accepted_voice->dialog_cache_key;
}

} // namespace creatures::voice
