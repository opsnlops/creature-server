
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/Types.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures :: ws {

    class StopPlaylistRequestDto : public oatpp::DTO {

        DTO_INIT(StopPlaylistRequestDto, DTO)

        DTO_FIELD_INFO(universe) {
            info->description = "Which universe to stop playback in?";
            info->required = true;
        }
        DTO_FIELD(UInt32, universe);
    };
}
#include OATPP_CODEGEN_END(DTO)

