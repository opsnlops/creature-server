#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "model/JsonCodec.h"
#include "server/namespace-stuffs.h"
#include "util/helpers.h"

namespace creatures::api {

inline constexpr std::size_t MAX_PLAYLIST_CONTROL_REQUEST_BODY_BYTES = 4096;
inline constexpr universe_t MIN_PLAYLIST_UNIVERSE = 1;
inline constexpr universe_t MAX_PLAYLIST_UNIVERSE = 63999;

struct StartPlaylistRequest {
    playlistId_t playlistId;
    universe_t universe;
};

struct StopPlaylistRequest {
    universe_t universe;
};

inline Result<StartPlaylistRequest> startPlaylistRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "playlist start request", {"playlist_id", "universe"});
    if (!fields.isSuccess())
        return Result<StartPlaylistRequest>{fields.getError().value()};
    auto playlistId = json_codec::requiredString(json, "playlist start request", "playlist_id", 36);
    auto universe =
        json_codec::requiredUnsigned<universe_t>(json, "playlist start request", "universe", MAX_PLAYLIST_UNIVERSE);
    if (!playlistId.isSuccess())
        return Result<StartPlaylistRequest>{playlistId.getError().value()};
    if (!universe.isSuccess())
        return Result<StartPlaylistRequest>{universe.getError().value()};
    if (universe.getValue().value() < MIN_PLAYLIST_UNIVERSE)
        return json_codec::invalid<StartPlaylistRequest>("playlist start request.universe must be at least 1");
    if (!isUuidShape(playlistId.getValue().value()))
        return json_codec::invalid<StartPlaylistRequest>("playlist start request.playlist_id must be a UUID");
    return Result<StartPlaylistRequest>{{playlistId.getValue().value(), universe.getValue().value()}};
}

inline Result<StopPlaylistRequest> stopPlaylistRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "playlist stop request", {"universe"});
    if (!fields.isSuccess())
        return Result<StopPlaylistRequest>{fields.getError().value()};
    auto universe =
        json_codec::requiredUnsigned<universe_t>(json, "playlist stop request", "universe", MAX_PLAYLIST_UNIVERSE);
    if (!universe.isSuccess())
        return Result<StopPlaylistRequest>{universe.getError().value()};
    if (universe.getValue().value() < MIN_PLAYLIST_UNIVERSE)
        return json_codec::invalid<StopPlaylistRequest>("playlist stop request.universe must be at least 1");
    return Result<StopPlaylistRequest>{{universe.getValue().value()}};
}

} // namespace creatures::api
