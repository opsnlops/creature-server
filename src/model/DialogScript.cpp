
#include <spdlog/spdlog.h>

#include <string>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>

#include "DialogScript.h"

namespace creatures {

oatpp::Object<DialogScriptDto> convertToDto(const DialogScript &script) {
    auto dto = DialogScriptDto::createShared();
    dto->id = script.id;
    dto->title = script.title;
    dto->notes = script.notes;
    dto->created_at = script.created_at;
    dto->updated_at = script.updated_at;
    // Omitted when unset so a client can tell "no stage bound" from "bound to
    // the empty string" (#123).
    if (!script.stage_id.empty()) {
        dto->stage_id = script.stage_id;
    }
    if (script.accepted_voice) {
        auto voice = AcceptedVoiceDto::createShared();
        voice->generation_id = script.accepted_voice->generation_id;
        voice->dialog_cache_key = script.accepted_voice->dialog_cache_key;
        voice->sound_file = script.accepted_voice->sound_file;
        voice->accepted_at = script.accepted_voice->accepted_at;
        dto->accepted_voice = voice;
    }
    if (script.background_music) {
        auto music = DialogBackgroundMusicDto::createShared();
        music->sound_file = script.background_music->sound_file;
        music->generation_id = script.background_music->generation_id;
        music->prompt = script.background_music->prompt;
        music->accepted_at = script.background_music->accepted_at;
        // #136: which voice take this was composed against. Omitted rather than
        // sent empty when unrecorded, so the client can tell "unknown" from
        // "known to be nothing" and show no verdict instead of a false one.
        if (!script.background_music->source_dialog_generation_id.empty())
            music->source_dialog_generation_id = script.background_music->source_dialog_generation_id;
        if (!script.background_music->source_dialog_cache_key.empty())
            music->source_dialog_cache_key = script.background_music->source_dialog_cache_key;
        dto->background_music = music;
    }

    auto turns = oatpp::List<oatpp::Object<DialogScriptTurnDto>>::createShared();
    for (const auto &t : script.turns) {
        auto td = DialogScriptTurnDto::createShared();
        td->creature_id = t.creature_id;
        td->text = t.text;
        turns->push_back(td);
    }
    dto->turns = turns;
    return dto;
}

DialogScript convertFromDto(const std::shared_ptr<DialogScriptDto> &scriptDto) {
    DialogScript script;
    if (!scriptDto) {
        return script;
    }
    if (scriptDto->id)
        script.id = scriptDto->id;
    if (scriptDto->title)
        script.title = scriptDto->title;
    if (scriptDto->notes)
        script.notes = scriptDto->notes;
    if (scriptDto->created_at)
        script.created_at = *scriptDto->created_at;
    if (scriptDto->updated_at)
        script.updated_at = *scriptDto->updated_at;
    if (scriptDto->stage_id)
        script.stage_id = scriptDto->stage_id;
    if (scriptDto->accepted_voice) {
        AcceptedVoice voice;
        if (scriptDto->accepted_voice->generation_id)
            voice.generation_id = scriptDto->accepted_voice->generation_id;
        if (scriptDto->accepted_voice->dialog_cache_key)
            voice.dialog_cache_key = scriptDto->accepted_voice->dialog_cache_key;
        if (scriptDto->accepted_voice->sound_file)
            voice.sound_file = scriptDto->accepted_voice->sound_file;
        if (scriptDto->accepted_voice->accepted_at)
            voice.accepted_at = *scriptDto->accepted_voice->accepted_at;
        script.accepted_voice = std::move(voice);
    }
    if (scriptDto->background_music) {
        DialogBackgroundMusic music;
        const auto &dto = scriptDto->background_music;
        if (dto->sound_file)
            music.sound_file = dto->sound_file;
        if (dto->generation_id)
            music.generation_id = dto->generation_id;
        if (dto->prompt)
            music.prompt = dto->prompt;
        if (dto->accepted_at)
            music.accepted_at = *dto->accepted_at;
        if (dto->source_dialog_generation_id)
            music.source_dialog_generation_id = dto->source_dialog_generation_id;
        if (dto->source_dialog_cache_key)
            music.source_dialog_cache_key = dto->source_dialog_cache_key;
        script.background_music = std::move(music);
    }
    if (scriptDto->turns) {
        for (const auto &td : *scriptDto->turns) {
            if (!td)
                continue;
            DialogScriptTurn t;
            if (td->creature_id)
                t.creature_id = td->creature_id;
            if (td->text)
                t.text = td->text;
            script.turns.push_back(std::move(t));
        }
    }
    return script;
}

nlohmann::json dialogScriptToJson(const DialogScript &script) {
    nlohmann::json j;
    j["id"] = script.id;
    j["title"] = script.title;
    j["notes"] = script.notes;
    j["created_at"] = script.created_at;
    j["updated_at"] = script.updated_at;
    if (!script.stage_id.empty()) {
        j["stage_id"] = script.stage_id;
    }
    if (script.accepted_voice) {
        j["accepted_voice"] = {{"generation_id", script.accepted_voice->generation_id},
                               {"dialog_cache_key", script.accepted_voice->dialog_cache_key},
                               {"sound_file", script.accepted_voice->sound_file},
                               {"accepted_at", script.accepted_voice->accepted_at}};
    }
    if (script.background_music) {
        j["background_music"] = {{"sound_file", script.background_music->sound_file},
                                 {"generation_id", script.background_music->generation_id},
                                 {"prompt", script.background_music->prompt},
                                 {"accepted_at", script.background_music->accepted_at}};
        if (!script.background_music->source_dialog_generation_id.empty()) {
            j["background_music"]["source_dialog_generation_id"] = script.background_music->source_dialog_generation_id;
        }
        if (!script.background_music->source_dialog_cache_key.empty()) {
            j["background_music"]["source_dialog_cache_key"] = script.background_music->source_dialog_cache_key;
        }
    }
    nlohmann::json turns = nlohmann::json::array();
    for (const auto &t : script.turns) {
        turns.push_back({{"creature_id", t.creature_id}, {"text", t.text}});
    }
    j["turns"] = turns;
    return j;
}

} // namespace creatures
