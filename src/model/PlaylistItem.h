
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

/*
 * This is one item in a playlist
 */

struct PlaylistItem {

    /**
     * The ID of the playlist
     */
    animationId_t animation_id;

    /**
     * The weight of the item
     */
    uint32_t weight;
};

#include OATPP_CODEGEN_BEGIN(DTO)

class PlaylistItemDto : public oatpp::DTO {

    DTO_INIT(PlaylistItemDto, DTO /* extends */)

    DTO_FIELD_INFO(animation_id) { info->description = "The ID of the animation for this entry"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(weight) { info->description = "This item's weight"; }
    DTO_FIELD(UInt32, weight);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<PlaylistItemDto> convertToDto(const PlaylistItem &playlistItem);
creatures::PlaylistItem convertFromDto(const std::shared_ptr<PlaylistItemDto> &playlistItemDto);

} // namespace creatures