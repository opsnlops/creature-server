
#pragma once

#include <cstdint>
#include <string_view>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/*
 * This is one item in a playlist
 */

struct PlaylistItem {
    animationId_t animation_id;
    uint32_t weight{0};

    bool operator==(const PlaylistItem &) const = default;
};

inline constexpr uint32_t MAX_PLAYLIST_ITEM_WEIGHT = 999;

nlohmann::json playlistItemToJson(const PlaylistItem &playlistItem);
Result<PlaylistItem> playlistItemFromJson(const nlohmann::json &json, std::string_view path = "playlist_item");

} // namespace creatures
