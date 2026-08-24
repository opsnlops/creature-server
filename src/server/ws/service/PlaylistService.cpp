#include "server/ws/service/PlaylistService.h"

#include <memory>
#include <string>
#include <utility>

#include "server/animation/SessionManager.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/storage/Storage.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SessionManager> sessionManager;
} // namespace creatures

namespace creatures::ws {

namespace {

template <typename T> Result<T> unavailable(const std::shared_ptr<OperationSpan> &span, const std::string &message) {
    const ServerError error{ServerError::InternalError, message};
    recordSpanError(span, error.getMessage(), "InternalError", error.getCode());
    return Result<T>{error};
}

template <typename T>
Result<T> propagateError(const std::shared_ptr<OperationSpan> &span, const ServerError &error, const char *type) {
    recordSpanError(span, error.getMessage(), type, error.getCode());
    return Result<T>{error};
}

PlaylistStatus stoppedStatus(universe_t universe) { return PlaylistStatus{universe, "", false, ""}; }

} // namespace

Result<std::vector<Playlist>> PlaylistService::getAllPlaylists(std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("PlaylistService.getAllPlaylists", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "getAllPlaylists");
    }
    if (!db)
        return unavailable<std::vector<Playlist>>(span, "Playlist database unavailable");

    auto result = db->getAllPlaylists(span);
    if (!result.isSuccess())
        return propagateError<std::vector<Playlist>>(span, result.getError().value(), "PlaylistLookupFailed");
    const auto playlists = result.getValue().value();
    if (playlists.empty()) {
        const ServerError error{ServerError::NotFound, "No playlists found"};
        return propagateError<std::vector<Playlist>>(span, error, "NotFound");
    }
    if (span) {
        span->setAttribute("playlists.count", static_cast<int64_t>(playlists.size()));
        span->setSuccess();
    }
    return Result<std::vector<Playlist>>{playlists};
}

Result<Playlist> PlaylistService::getPlaylist(const playlistId_t &playlistId, std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability ? observability->createOperationSpan("PlaylistService.getPlaylist", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "getPlaylist");
        span->setAttribute("playlist.id", playlistId);
    }
    if (!db)
        return unavailable<Playlist>(span, "Playlist database unavailable");
    if (playlistId.empty()) {
        const ServerError error{ServerError::InvalidData, "playlistId is required"};
        return propagateError<Playlist>(span, error, "InvalidData");
    }
    auto result = db->getPlaylist(playlistId, span);
    if (!result.isSuccess())
        return propagateError<Playlist>(span, result.getError().value(), "PlaylistLookupFailed");
    const auto playlist = result.getValue().value();
    if (span) {
        span->setAttribute("playlist.name", playlist.name);
        span->setSuccess();
    }
    return Result<Playlist>{playlist};
}

Result<Playlist> PlaylistService::upsertPlaylist(const std::string &playlistJson,
                                                 std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("PlaylistService.upsertPlaylist", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "upsertPlaylist");
        span->setAttribute("json.size", static_cast<int64_t>(playlistJson.size()));
    }
    if (!db)
        return unavailable<Playlist>(span, "Playlist database unavailable");

    auto validationSpan =
        observability ? observability->createChildOperationSpan("PlaylistService.validateJson", span) : nullptr;
    auto jsonResult = JsonParser::parseApiJsonString(playlistJson, "playlist upsert validation", validationSpan);
    if (!jsonResult.isSuccess())
        return propagateError<Playlist>(span, jsonResult.getError().value(), "JSONParseError");
    auto validation = db->validatePlaylistJson(jsonResult.getValue().value());
    if (!validation.isSuccess()) {
        const ServerError error{ServerError::InvalidData, validation.getError()->getMessage()};
        recordSpanError(validationSpan, error.getMessage(), "InvalidData", error.getCode());
        return propagateError<Playlist>(span, error, "InvalidData");
    }
    if (validationSpan)
        validationSpan->setSuccess();

    auto result = storage::publishPlaylist(playlistJson, span);
    if (!result.isSuccess())
        return propagateError<Playlist>(span, result.getError().value(), "DatabaseError");
    if (!result.getValue().has_value()) {
        const ServerError error{ServerError::InternalError, "Database returned no playlist after upsert"};
        return propagateError<Playlist>(span, error, "MissingDatabaseResult");
    }
    const auto playlist = result.getValue().value();
    if (span) {
        span->setAttribute("playlist.id", playlist.id);
        span->setAttribute("playlist.name", playlist.name);
        span->setSuccess();
    }
    return Result<Playlist>{playlist};
}

Result<api::StatusResponse> PlaylistService::startPlaylist(universe_t universe, const playlistId_t &playlistId,
                                                           std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("PlaylistService.startPlaylist", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "startPlaylist");
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("playlist.id", playlistId);
    }
    if (!db || !sessionManager || !eventLoop)
        return unavailable<api::StatusResponse>(span, "Playlist scheduler unavailable");
    if (playlistId.empty()) {
        const ServerError error{ServerError::InvalidData, "No playlist ID provided"};
        return propagateError<api::StatusResponse>(span, error, "InvalidData");
    }
    auto playlistResult = db->getPlaylist(playlistId, span);
    if (!playlistResult.isSuccess())
        return propagateError<api::StatusResponse>(span, playlistResult.getError().value(), "PlaylistLookupFailed");
    const auto playlist = playlistResult.getValue().value();
    if (span) {
        span->setAttribute("playlist.name", playlist.name);
        span->setAttribute("playlist.items", static_cast<int64_t>(playlist.number_of_items));
    }

    if (auto existingSession = sessionManager->getCurrentSession(universe);
        existingSession && !existingSession->isCancelled())
        existingSession->cancel();
    const PlaylistStatus status{universe, playlistId, true, ""};
    sessionManager->startPlaylist(universe, playlistId);
    sessionManager->setPlaylistStatus(universe, status);
    eventLoop->scheduleEvent(std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe));
    if (metrics)
        metrics->incrementPlaylistsStarted();

    std::optional<std::string> sessionId;
    if (auto session = sessionManager->getCurrentSession(universe)) {
        sessionId = session->getSessionId();
        if (span)
            span->setAttribute("session.id", *sessionId);
    }
    if (span)
        span->setSuccess();
    return Result<api::StatusResponse>{api::makeStatusResponse(200, "Started playback", api::STATUS_OK, sessionId)};
}

Result<api::StatusResponse> PlaylistService::stopPlaylist(universe_t universe,
                                                          std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("PlaylistService.stopPlaylist", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "stopPlaylist");
        span->setAttribute("universe", static_cast<int64_t>(universe));
    }
    if (!sessionManager)
        return unavailable<api::StatusResponse>(span, "Playlist scheduler unavailable");

    std::optional<std::string> sessionId;
    if (auto session = sessionManager->getCurrentSession(universe))
        sessionId = session->getSessionId();
    sessionManager->stopPlaylist(universe);
    sessionManager->clearPlaylist(universe);
    broadcastPlaylistStatusToAllClients(stoppedStatus(universe));
    if (span)
        span->setSuccess();
    return Result<api::StatusResponse>{api::makeStatusResponse(200, "Stopped playback", api::STATUS_OK, sessionId)};
}

Result<PlaylistStatus> PlaylistService::playlistStatus(universe_t universe, std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("PlaylistService.playlistStatus", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "playlistStatus");
        span->setAttribute("universe", static_cast<int64_t>(universe));
    }
    const auto status = sessionManager ? sessionManager->getPlaylistStatus(universe) : std::nullopt;
    if (span)
        span->setSuccess();
    return Result<PlaylistStatus>{status.value_or(stoppedStatus(universe))};
}

Result<std::vector<PlaylistStatus>> PlaylistService::getAllPlaylistStatuses(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability ? observability->createOperationSpan("PlaylistService.getAllPlaylistStatuses", parentSpan)
                              : nullptr;
    if (span) {
        span->setAttribute("service", "PlaylistService");
        span->setAttribute("operation", "getAllPlaylistStatuses");
    }
    const auto statuses = sessionManager ? sessionManager->getAllPlaylistStatuses() : std::vector<PlaylistStatus>{};
    if (span) {
        span->setAttribute("playlists.count", static_cast<int64_t>(statuses.size()));
        span->setSuccess();
    }
    return Result<std::vector<PlaylistStatus>>{statuses};
}

} // namespace creatures::ws
