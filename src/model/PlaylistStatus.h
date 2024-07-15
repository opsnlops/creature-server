
#pragma once

#include <string>


#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"


namespace creatures {

    struct PlaylistStatus {

        /**
         * Which universe?
         */
        universe_t universe;

        /**
         * Which playlist, if any?
         */
        playlistId_t playlist;

        /**
         * True if playing
         */
        bool playing;

        /**
         * Which animation is currently playing?
         */
        animationId_t current_animation;
    };


#include OATPP_CODEGEN_BEGIN(DTO)

class PlaylistStatusDto : public oatpp::DTO {

    DTO_INIT(PlaylistStatusDto, DTO /* extends */)

    DTO_FIELD_INFO(universe) {
        info->description = "Universe ID";
    }
    DTO_FIELD(UInt32, universe);

    DTO_FIELD_INFO(playlist) {
        info->description = "Currently active playlist";
    }
    DTO_FIELD(String, playlist);

    DTO_FIELD_INFO(playing) {
        info->description = "Is a playlist currently running?";
    }
    DTO_FIELD(Boolean, playing);

    DTO_FIELD_INFO(current_animation) {
        info->description = "Currently playing animation";
    }
    DTO_FIELD(String, current_animation);

};

#include OATPP_CODEGEN_END(DTO)

    oatpp::Object<PlaylistStatusDto> convertToDto(const PlaylistStatus &playlistStatus);
    creatures::PlaylistStatus convertFromDto(const std::shared_ptr<PlaylistStatusDto> &playlistStatusDto);

}