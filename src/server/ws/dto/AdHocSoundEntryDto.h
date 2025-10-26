#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/Sound.h"

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

class AdHocSoundEntryDto : public oatpp::DTO {

    DTO_INIT(AdHocSoundEntryDto, DTO)

    DTO_FIELD_INFO(animation_id) { info->description = "Animation ID referencing this ad-hoc sound"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(created_at) { info->description = "Creation timestamp in ISO8601 format"; }
    DTO_FIELD(String, created_at);

    DTO_FIELD_INFO(sound_file) { info->description = "Absolute path to the generated WAV file"; }
    DTO_FIELD(String, sound_file);

    DTO_FIELD_INFO(sound) { info->description = "Standard Sound DTO for client reuse"; }
    DTO_FIELD(Object<creatures::SoundDto>, sound);
};

class AdHocSoundListDto : public oatpp::DTO {

    DTO_INIT(AdHocSoundListDto, DTO)

    DTO_FIELD_INFO(count) { info->description = "Number of ad-hoc sounds returned"; }
    DTO_FIELD(UInt32, count);

    DTO_FIELD_INFO(items) { info->description = "Ad-hoc sound entries"; }
    DTO_FIELD(List<Object<AdHocSoundEntryDto>>, items);
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
