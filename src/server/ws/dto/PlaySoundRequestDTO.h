
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures :: ws {

    class PlaySoundRequestDTO : public oatpp::DTO {

        DTO_INIT(PlaySoundRequestDTO, DTO)

        DTO_FIELD_INFO(file_name) {
            info->description = "The file name to play";
            info->required = true;
        }
        DTO_FIELD(String, file_name);
    };
}
#include OATPP_CODEGEN_END(DTO)

