
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "model/PlaylistItem.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

struct Playlist {
    playlistId_t id;
    std::string name;
    std::vector<PlaylistItem> items;
    uint32_t number_of_items;
};

inline constexpr std::size_t MAX_PLAYLIST_REQUEST_BODY_BYTES = 1024ULL * 1024ULL;
inline constexpr std::size_t MAX_PLAYLIST_NAME_BYTES = 128;
inline constexpr std::size_t MAX_PLAYLIST_ITEMS = 256;

Result<uint64_t> playlistTotalWeight(const Playlist &playlist);
Result<animationId_t> playlistAnimationAtWeight(const Playlist &playlist, uint64_t selectedWeight);
nlohmann::json playlistToJson(const Playlist &playlist);
Result<Playlist> playlistFromJson(const nlohmann::json &json, std::string_view path = "playlist");

} // namespace creatures
