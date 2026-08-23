
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

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

inline constexpr std::size_t MAX_PLAYLIST_STATUS_ID_BYTES = 36;

nlohmann::json playlistStatusToJson(const PlaylistStatus &playlistStatus);
Result<PlaylistStatus> playlistStatusFromJson(const nlohmann::json &json, std::string_view path = "playlist_status");

} // namespace creatures
