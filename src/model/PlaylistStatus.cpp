
#include <limits>

#include "model/JsonCodec.h"
#include "model/PlaylistStatus.h"

namespace creatures {

namespace {

template <typename T> Result<PlaylistStatus> forwardPlaylistStatusError(const Result<T> &result) {
    return Result<PlaylistStatus>{result.getError().value()};
}

} // namespace

nlohmann::json playlistStatusToJson(const PlaylistStatus &playlistStatus) {
    return {{"universe", playlistStatus.universe},
            {"playlist", playlistStatus.playlist},
            {"playing", playlistStatus.playing},
            {"current_animation", playlistStatus.current_animation}};
}

Result<PlaylistStatus> playlistStatusFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"universe", "playlist", "playing", "current_animation"});
    if (!fields.isSuccess())
        return forwardPlaylistStatusError(fields);
    auto universe =
        json_codec::requiredUnsigned<uint32_t>(json, path, "universe", std::numeric_limits<uint32_t>::max());
    auto playlist = json_codec::requiredString(json, path, "playlist", MAX_PLAYLIST_STATUS_ID_BYTES, true);
    auto currentAnimation =
        json_codec::requiredString(json, path, "current_animation", MAX_PLAYLIST_STATUS_ID_BYTES, true);
    const auto playing = json.find("playing");
    if (!universe.isSuccess())
        return forwardPlaylistStatusError(universe);
    if (!playlist.isSuccess())
        return forwardPlaylistStatusError(playlist);
    if (!currentAnimation.isSuccess())
        return forwardPlaylistStatusError(currentAnimation);
    if (playing == json.end())
        return json_codec::invalid<PlaylistStatus>(fmt::format("{}.playing is required", path));
    if (!playing->is_boolean())
        return json_codec::invalid<PlaylistStatus>(fmt::format("{}.playing must be a boolean", path));

    return Result<PlaylistStatus>{PlaylistStatus{universe.getValue().value(), playlist.getValue().value(),
                                                 playing->get<bool>(), currentAnimation.getValue().value()}};
}

} // namespace creatures
