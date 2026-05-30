
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
    nlohmann::json turns = nlohmann::json::array();
    for (const auto &t : script.turns) {
        turns.push_back({{"creature_id", t.creature_id}, {"text", t.text}});
    }
    j["turns"] = turns;
    return j;
}

} // namespace creatures
