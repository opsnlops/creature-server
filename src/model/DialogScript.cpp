
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
    if (script.background_music) {
        auto music = DialogBackgroundMusicDto::createShared();
        music->sound_file = script.background_music->sound_file;
        music->generation_id = script.background_music->generation_id;
        music->prompt = script.background_music->prompt;
        music->accepted_at = script.background_music->accepted_at;
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
    if (script.background_music) {
        j["background_music"] = {{"sound_file", script.background_music->sound_file},
                                 {"generation_id", script.background_music->generation_id},
                                 {"prompt", script.background_music->prompt},
                                 {"accepted_at", script.background_music->accepted_at}};
    }
    nlohmann::json turns = nlohmann::json::array();
    for (const auto &t : script.turns) {
        turns.push_back({{"creature_id", t.creature_id}, {"text", t.text}});
    }
    j["turns"] = turns;
    return j;
}

} // namespace creatures
