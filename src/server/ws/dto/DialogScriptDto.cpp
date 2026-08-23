#include "server/ws/dto/DialogScriptDto.h"

namespace creatures {

oatpp::Object<DialogScriptDto> convertToDto(const DialogScript &script) {
    auto dto = DialogScriptDto::createShared();
    dto->id = script.id;
    dto->title = script.title;
    dto->notes = script.notes;
    dto->created_at = script.created_at;
    dto->updated_at = script.updated_at;
    if (!script.stage_id.empty())
        dto->stage_id = script.stage_id;
    if (script.accepted_voice) {
        auto v = AcceptedVoiceDto::createShared();
        v->generation_id = script.accepted_voice->generation_id;
        v->dialog_cache_key = script.accepted_voice->dialog_cache_key;
        v->sound_file = script.accepted_voice->sound_file;
        v->accepted_at = script.accepted_voice->accepted_at;
        dto->accepted_voice = v;
    }
    if (script.background_music) {
        auto m = DialogBackgroundMusicDto::createShared();
        m->sound_file = script.background_music->sound_file;
        m->generation_id = script.background_music->generation_id;
        m->prompt = script.background_music->prompt;
        m->accepted_at = script.background_music->accepted_at;
        if (!script.background_music->source_dialog_generation_id.empty())
            m->source_dialog_generation_id = script.background_music->source_dialog_generation_id;
        if (!script.background_music->source_dialog_cache_key.empty())
            m->source_dialog_cache_key = script.background_music->source_dialog_cache_key;
        dto->background_music = m;
    }
    dto->turns = oatpp::List<oatpp::Object<DialogScriptTurnDto>>::createShared();
    for (const auto &turn : script.turns) {
        auto t = DialogScriptTurnDto::createShared();
        t->creature_id = turn.creature_id;
        t->text = turn.text;
        dto->turns->push_back(t);
    }
    return dto;
}

DialogScript convertFromDto(const std::shared_ptr<DialogScriptDto> &dto) {
    DialogScript script;
    if (!dto)
        return script;
    if (dto->id)
        script.id = dto->id;
    if (dto->title)
        script.title = dto->title;
    if (dto->notes)
        script.notes = dto->notes;
    if (dto->created_at)
        script.created_at = *dto->created_at;
    if (dto->updated_at)
        script.updated_at = *dto->updated_at;
    if (dto->stage_id)
        script.stage_id = dto->stage_id;
    if (dto->accepted_voice) {
        AcceptedVoice v;
        if (dto->accepted_voice->generation_id)
            v.generation_id = dto->accepted_voice->generation_id;
        if (dto->accepted_voice->dialog_cache_key)
            v.dialog_cache_key = dto->accepted_voice->dialog_cache_key;
        if (dto->accepted_voice->sound_file)
            v.sound_file = dto->accepted_voice->sound_file;
        if (dto->accepted_voice->accepted_at)
            v.accepted_at = *dto->accepted_voice->accepted_at;
        script.accepted_voice = std::move(v);
    }
    if (dto->background_music) {
        DialogBackgroundMusic m;
        const auto &d = dto->background_music;
        if (d->sound_file)
            m.sound_file = d->sound_file;
        if (d->generation_id)
            m.generation_id = d->generation_id;
        if (d->prompt)
            m.prompt = d->prompt;
        if (d->accepted_at)
            m.accepted_at = *d->accepted_at;
        if (d->source_dialog_generation_id)
            m.source_dialog_generation_id = d->source_dialog_generation_id;
        if (d->source_dialog_cache_key)
            m.source_dialog_cache_key = d->source_dialog_cache_key;
        script.background_music = std::move(m);
    }
    if (dto->turns)
        for (const auto &d : *dto->turns) {
            if (!d)
                continue;
            DialogScriptTurn t;
            if (d->creature_id)
                t.creature_id = d->creature_id;
            if (d->text)
                t.text = d->text;
            script.turns.push_back(std::move(t));
        }
    return script;
}

} // namespace creatures
