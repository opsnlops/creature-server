#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/PlaylistItem.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class PlaylistItemDto : public oatpp::DTO {
    DTO_INIT(PlaylistItemDto, DTO)

    DTO_FIELD_INFO(animation_id) { info->description = "The UUID of the animation for this entry"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(weight) { info->description = "Relative selection weight from 1 through 999"; }
    DTO_FIELD(UInt32, weight);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<PlaylistItemDto> convertToDto(const PlaylistItem &playlistItem);
PlaylistItem convertFromDto(const oatpp::Object<PlaylistItemDto> &playlistItemDto);

} // namespace creatures
