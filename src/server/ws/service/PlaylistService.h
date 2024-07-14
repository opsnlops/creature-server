
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Playlist.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures :: ws {

    class PlaylistService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        static oatpp::Object<ListDto<oatpp::Object<creatures::PlaylistDto>>> getAllPlaylists();
        oatpp::Object<creatures::PlaylistDto> getPlaylist(const oatpp::String& playlistId);
        oatpp::Object<creatures::PlaylistDto> upsertPlaylist(const std::string& playlistJson);


        /**
         * Play a single animation on one universe out of the database
         *
         * @param animationId the animation to play
         * @param universe which universe to play the animation in
         * @return The status of what happened
         */
       // oatpp::Object<creatures::ws::StatusDto> playStoredAnimation(const oatpp::String& animationId, universe_t universe);
    };


} // creatures :: ws
