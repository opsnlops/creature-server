#include "server/ws/dto/PlaylistItemDto.h"

namespace creatures {

oatpp::Object<PlaylistItemDto> convertToDto(const PlaylistItem &playlistItem) {
    auto dto = PlaylistItemDto::createShared();
    dto->animation_id = playlistItem.animation_id;
    dto->weight = playlistItem.weight;
    return dto;
}

PlaylistItem convertFromDto(const oatpp::Object<PlaylistItemDto> &playlistItemDto) {
    PlaylistItem playlistItem;
    playlistItem.animation_id = playlistItemDto->animation_id;
    playlistItem.weight = playlistItemDto->weight;
    return playlistItem;
}

} // namespace creatures
