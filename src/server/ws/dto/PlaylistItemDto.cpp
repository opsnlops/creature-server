#include "server/ws/dto/PlaylistItemDto.h"

#include "server/ws/dto/PlaylistDto.h"

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

oatpp::Object<PlaylistDto> convertToDto(const Playlist &playlist) {
    auto dto = PlaylistDto::createShared();
    dto->id = playlist.id;
    dto->name = playlist.name;
    dto->number_of_items = playlist.number_of_items;
    dto->items = oatpp::List<oatpp::Object<PlaylistItemDto>>::createShared();
    for (const auto &item : playlist.items)
        dto->items->emplace_back(convertToDto(item));
    return dto;
}

Playlist convertFromDto(const oatpp::Object<PlaylistDto> &dto) {
    Playlist playlist;
    playlist.id = dto->id;
    playlist.name = dto->name;
    playlist.number_of_items = dto->number_of_items;
    if (dto->items) {
        playlist.items.reserve(dto->items->size());
        for (const auto &item : *dto->items)
            playlist.items.push_back(convertFromDto(item));
    }
    return playlist;
}

} // namespace creatures
