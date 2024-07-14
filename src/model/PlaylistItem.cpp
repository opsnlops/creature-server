

#include <string>

#include <spdlog/spdlog.h>
#include <oatpp/core/Types.hpp>

#include "model/PlaylistItem.h"

namespace creatures {

    std::vector<std::string> playlistitem_required_fields = {
            "animation_id", "weight"
    };


    // Convert a DTO to the real thing
    PlaylistItem convertFromDto(const std::shared_ptr<PlaylistItemDto> &playlistItemDto) {

        trace("Converting PlaylistItemDto to PlaylistItem");

        PlaylistItem playlistItem;
        playlistItem.animation_id = playlistItemDto->animation_id;
        playlistItem.weight = playlistItemDto->weight;
        trace("animation_id: {}, weight: {}", playlistItem.animation_id, playlistItem.weight);

        return playlistItem;
    }

    oatpp::Object<PlaylistItemDto> convertToDto(const PlaylistItem &playlistItem) {
        auto playlistItemDto = PlaylistItemDto::createShared();
        playlistItemDto->animation_id = playlistItem.animation_id;
        playlistItemDto->weight = playlistItem.weight;
        return playlistItemDto;
    }

}