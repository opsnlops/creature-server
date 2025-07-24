
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Playlist.h"
#include "model/PlaylistStatus.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures ::ws {

class PlaylistService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    static oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistDto>>> getAllPlaylists();
    static oatpp::Object<creatures::PlaylistDto> getPlaylist(const oatpp::String &playlistId);
    static oatpp::Object<creatures::PlaylistDto> upsertPlaylist(const std::string &playlistJson);

    static oatpp::Object<creatures::ws::StatusDto> startPlaylist(universe_t universe, const oatpp::String &playlistId);
    static oatpp::Object<creatures::ws::StatusDto> stopPlaylist(universe_t universe);
    static oatpp::Object<creatures::PlaylistStatusDto> playlistStatus(universe_t universe);

    static oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistStatusDto>>> getAllPlaylistStatuses();
};

} // namespace creatures::ws
