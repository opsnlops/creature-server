#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

class DialogMusicRequestDto : public oatpp::DTO {
    DTO_INIT(DialogMusicRequestDto, DTO)

    DTO_FIELD(String, script_id);
    DTO_FIELD(String, dialog_cache_key);
    DTO_FIELD(String, dialog_generation_id);
    DTO_FIELD(String, prompt);

    DTO_FIELD_INFO(duration_extension_ms) {
        info->description =
            "Extra instrumental material to request beyond the exact dialog duration. Defaults to 0; range "
            "0...60000 ms. Final rendering continues until the later of the dialog or accepted music.";
        info->required = false;
    }
    DTO_FIELD(Int64, duration_extension_ms);

    DTO_FIELD_INFO(generation_mode) {
        info->description = "ElevenLabs Music mode: track, loop, or ambience. Defaults to track.";
        info->required = false;
    }
    DTO_FIELD(String, generation_mode);
};

class DialogMusicGenerationResultDto : public oatpp::DTO {
    DTO_INIT(DialogMusicGenerationResultDto, DTO)

    DTO_FIELD(String, music_generation_id);
    DTO_FIELD(String, mp3_url);
    DTO_FIELD(Float64, duration_seconds);
    DTO_FIELD(Int64, dialog_duration_ms);
    DTO_FIELD(Int64, duration_extension_ms);
    DTO_FIELD(Int64, requested_music_length_ms);
    DTO_FIELD(String, prompt);
};

class DialogMusicPromotionResultDto : public oatpp::DTO {
    DTO_INIT(DialogMusicPromotionResultDto, DTO)

    DTO_FIELD(String, music_generation_id);
    DTO_FIELD(String, sound_file);
    DTO_FIELD(String, mp3_url);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
