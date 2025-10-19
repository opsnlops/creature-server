#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures::ws {

/**
 * @brief DTO for requesting lip sync generation from a sound file using Rhubarb
 */
class GenerateLipSyncRequestDto : public oatpp::DTO {

    DTO_INIT(GenerateLipSyncRequestDto, DTO)

    DTO_FIELD_INFO(sound_file) {
        info->description = "The filename of the sound file to process (must be a .wav file)";
        info->required = true;
    }
    DTO_FIELD(String, sound_file);

    DTO_FIELD_INFO(allow_overwrite) {
        info->description = "Allow overwriting an existing JSON file if it already exists (default: false)";
        info->required = false;
    }
    DTO_FIELD(Boolean, allow_overwrite) = false;
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(DTO)
