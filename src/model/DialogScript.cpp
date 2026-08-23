#include "model/DialogScript.h"

namespace creatures {

nlohmann::json dialogScriptToJson(const DialogScript &script) {
    nlohmann::json json{{"id", script.id},
                        {"title", script.title},
                        {"notes", script.notes},
                        {"created_at", script.created_at},
                        {"updated_at", script.updated_at}};
    if (!script.stage_id.empty())
        json["stage_id"] = script.stage_id;
    if (script.accepted_voice)
        json["accepted_voice"] = {{"generation_id", script.accepted_voice->generation_id},
                                  {"dialog_cache_key", script.accepted_voice->dialog_cache_key},
                                  {"sound_file", script.accepted_voice->sound_file},
                                  {"accepted_at", script.accepted_voice->accepted_at}};
    if (script.background_music) {
        auto &music = json["background_music"] = {{"sound_file", script.background_music->sound_file},
                                                  {"generation_id", script.background_music->generation_id},
                                                  {"prompt", script.background_music->prompt},
                                                  {"accepted_at", script.background_music->accepted_at}};
        if (!script.background_music->source_dialog_generation_id.empty())
            music["source_dialog_generation_id"] = script.background_music->source_dialog_generation_id;
        if (!script.background_music->source_dialog_cache_key.empty())
            music["source_dialog_cache_key"] = script.background_music->source_dialog_cache_key;
    }
    json["turns"] = nlohmann::json::array();
    for (const auto &turn : script.turns)
        json["turns"].push_back({{"creature_id", turn.creature_id}, {"text", turn.text}});
    return json;
}

} // namespace creatures
