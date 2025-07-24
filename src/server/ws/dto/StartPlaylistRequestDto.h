
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include OATPP_CODEGEN_BEGIN(DTO)

namespace creatures ::ws {

class StartPlaylistRequestDto : public oatpp::DTO {

    DTO_INIT(StartPlaylistRequestDto, DTO)

    DTO_FIELD_INFO(playlist_id) {
        info->description = "The playlist to play";
        info->required = true;
    }
    DTO_FIELD(String, playlist_id);

    DTO_FIELD_INFO(universe) {
        info->description = "Which universe to play the playlist in";
        info->required = true;
    }
    DTO_FIELD(UInt32, universe);
};
} // namespace creatures::ws
#include OATPP_CODEGEN_END(DTO)
