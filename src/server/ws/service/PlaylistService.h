#pragma once

#include <memory>
#include <string>
#include <vector>

#include "api/JsonResponse.h"
#include "model/Playlist.h"
#include "model/PlaylistStatus.h"
#include "util/Result.h"

namespace creatures {
class RequestSpan;
}

namespace creatures::ws {

class PlaylistService {
  public:
    static Result<std::vector<Playlist>> getAllPlaylists(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<Playlist> getPlaylist(const playlistId_t &playlistId,
                                        std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<Playlist> upsertPlaylist(const std::string &playlistJson,
                                           std::shared_ptr<RequestSpan> parentSpan = nullptr);

    static Result<api::StatusResponse> startPlaylist(universe_t universe, const playlistId_t &playlistId,
                                                     std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<api::StatusResponse> stopPlaylist(universe_t universe,
                                                    std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<PlaylistStatus> playlistStatus(universe_t universe,
                                                 std::shared_ptr<RequestSpan> parentSpan = nullptr);
    static Result<std::vector<PlaylistStatus>>
    getAllPlaylistStatuses(std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
