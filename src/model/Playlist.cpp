
#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>

#include "model/Playlist.h"
#include "model/PlaylistItem.h"

namespace creatures {


    std::vector<std::string> playlist_required_fields = {
            "id", "number_of_items", "items"
    };

     std::vector<std::string> playlistitems_required_fields = {
            "animation_id", "weight"
    };


    oatpp::Object<PlaylistDto> convertToDto(const Playlist &playlist) {
        auto playlistDto = PlaylistDto::createShared();
        playlistDto->id = playlist.id;
        playlistDto->name = playlist.name;
        playlistDto->number_of_items = playlist.number_of_items;
        playlistDto->items = oatpp::List<oatpp::Object<PlaylistItemDto>>::createShared();

        // Now do the items
        for (const auto &item: playlist.items) {
            playlistDto->items->emplace_back(convertToDto(item));
        }

        return playlistDto;
    }

    Playlist convertFromDto(const std::shared_ptr<PlaylistDto> &playlistDto) {
        Playlist playlist;
        playlist.id = playlistDto->id;
        playlist.name = playlistDto->name;
        playlist.number_of_items = playlistDto->number_of_items;

        // Ensure the list is initialized before iterating
        playlist.items = std::vector<PlaylistItem>();
        for (const auto &listItem: *playlistDto->items.getPtr()) {
            playlist.items.push_back(convertFromDto(listItem.getPtr()));
        }

        return playlist;
    }

}