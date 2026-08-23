#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/DialogScript.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class DialogScriptTurnDto : public oatpp::DTO {
    DTO_INIT(DialogScriptTurnDto, DTO)
    DTO_FIELD(String, creature_id);
    DTO_FIELD(String, text);
};

class AcceptedVoiceDto : public oatpp::DTO {
    DTO_INIT(AcceptedVoiceDto, DTO)
    DTO_FIELD(String, generation_id);
    DTO_FIELD(String, dialog_cache_key);
    DTO_FIELD(String, sound_file);
    DTO_FIELD(Int64, accepted_at);
};

class DialogBackgroundMusicDto : public oatpp::DTO {
    DTO_INIT(DialogBackgroundMusicDto, DTO)
    DTO_FIELD(String, sound_file);
    DTO_FIELD(String, generation_id);
    DTO_FIELD(String, prompt);
    DTO_FIELD(Int64, accepted_at);
    DTO_FIELD(String, source_dialog_generation_id);
    DTO_FIELD(String, source_dialog_cache_key);
};

class DialogScriptDto : public oatpp::DTO {
    DTO_INIT(DialogScriptDto, DTO)
    DTO_FIELD(String, stage_id);
    DTO_FIELD(String, id);
    DTO_FIELD(String, title);
    DTO_FIELD(String, notes);
    DTO_FIELD(List<Object<DialogScriptTurnDto>>, turns);
    DTO_FIELD(Object<DialogBackgroundMusicDto>, background_music);
    DTO_FIELD(Object<AcceptedVoiceDto>, accepted_voice);
    DTO_FIELD(Int64, created_at);
    DTO_FIELD(Int64, updated_at);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<DialogScriptDto> convertToDto(const DialogScript &script);
DialogScript convertFromDto(const std::shared_ptr<DialogScriptDto> &scriptDto);

} // namespace creatures
