
#include <string>

#include <oatpp/core/Types.hpp>

#include "model/PlaylistStatus.h"

namespace creatures {

PlaylistStatus convertFromDto(const std::shared_ptr<PlaylistStatusDto> &playlistStatusDto) {
    PlaylistStatus playlistStatus;
    playlistStatus.universe = playlistStatusDto->universe;
    playlistStatus.playlist = playlistStatusDto->playlist;
    playlistStatus.playing = playlistStatusDto->playing;
    playlistStatus.current_animation = playlistStatusDto->current_animation;

    return playlistStatus;
}

oatpp::Object<PlaylistStatusDto> convertToDto(const PlaylistStatus &playlistStatus) {
    auto playlistStatusDto = PlaylistStatusDto::createShared();
    playlistStatusDto->universe = playlistStatus.universe;
    playlistStatusDto->playlist = playlistStatus.playlist;
    playlistStatusDto->playing = playlistStatus.playing;
    playlistStatusDto->current_animation = playlistStatus.current_animation;

    return playlistStatusDto;
}

} // namespace creatures