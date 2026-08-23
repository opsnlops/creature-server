#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/PlaylistStatus.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class PlaylistStatusDto : public oatpp::DTO {
    DTO_INIT(PlaylistStatusDto, DTO)

    DTO_FIELD_INFO(universe) { info->description = "Universe ID"; }
    DTO_FIELD(UInt32, universe);

    DTO_FIELD_INFO(playlist) { info->description = "Currently active playlist"; }
    DTO_FIELD(String, playlist);

    DTO_FIELD_INFO(playing) { info->description = "Is a playlist currently running?"; }
    DTO_FIELD(Boolean, playing);

    DTO_FIELD_INFO(current_animation) { info->description = "Currently playing animation"; }
    DTO_FIELD(String, current_animation);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<PlaylistStatusDto> convertToDto(const PlaylistStatus &playlistStatus);
PlaylistStatus convertFromDto(const oatpp::Object<PlaylistStatusDto> &playlistStatusDto);

} // namespace creatures
