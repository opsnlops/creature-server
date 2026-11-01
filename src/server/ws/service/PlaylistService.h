#pragma once

#include <memory>
#include <string>
#include <vector>

#include "api/JsonResponse.h"
#include "model/Playlist.h"
#include "model/PlaylistStatus.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

class PlaylistService {
  public:
    static Result<std::vector<Playlist>> getAllPlaylists(SpanParent parentSpan = nullptr);
    static Result<Playlist> getPlaylist(const playlistId_t &playlistId, SpanParent parentSpan = nullptr);
    static Result<Playlist> upsertPlaylist(const std::string &playlistJson, SpanParent parentSpan = nullptr);

    static Result<api::StatusResponse> startPlaylist(universe_t universe, const playlistId_t &playlistId,
                                                     SpanParent parentSpan = nullptr);
    static Result<api::StatusResponse> stopPlaylist(universe_t universe, SpanParent parentSpan = nullptr);
    static Result<PlaylistStatus> playlistStatus(universe_t universe, SpanParent parentSpan = nullptr);
    static Result<std::vector<PlaylistStatus>> getAllPlaylistStatuses(SpanParent parentSpan = nullptr);
};

} // namespace creatures::ws
