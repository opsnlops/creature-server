#include "server/transport/PlaylistHandlers.h"

#include <charconv>
#include <cstdint>

#include "api/JsonResponse.h"
#include "api/PlaylistRequests.h"
#include "model/Playlist.h"
#include "model/PlaylistStatus.h"
#include "server/ws/service/PlaylistService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::transport {
namespace {

const char *serverErrorType(const ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::Unauthorized:
        return "Unauthorized";
    case ServerError::Forbidden:
        return "Forbidden";
    case ServerError::InvalidData:
        return "InvalidData";
    case ServerError::DatabaseError:
        return "DatabaseError";
    case ServerError::Conflict:
        return "Conflict";
    default:
        return "InternalError";
    }
}

PreparedResponse errorResponse(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    const auto status = serverErrorToStatusCode(error.getCode());
    recordSpanError(span, error.getMessage(), serverErrorType(error.getCode()), error.getCode());
    return PreparedResponse::json(
        status, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(status, error.getMessage()))));
}

template <typename T, typename Serializer>
PreparedResponse resultResponse(const Result<T> &result, const std::shared_ptr<OperationSpan> &span,
                                Serializer serializer) {
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(serializer(result.getValue().value())));
}

std::shared_ptr<OperationSpan> parseSpan(const std::string &name, const std::shared_ptr<OperationSpan> &parent) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, parent) : nullptr;
}

} // namespace

PreparedResponse listPlaylists(const std::shared_ptr<OperationSpan> &span) {
    return resultResponse(creatures::ws::PlaylistService::getAllPlaylists(), span,
                          [](const auto &playlists) { return api::listResponseToJson(playlists, playlistToJson); });
}

PreparedResponse getPlaylist(const std::string &playlistId, const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        span->setAttribute("playlist.id", playlistId);
    }
    if (!isUuidShape(playlistId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "playlistId must be a UUID"), span);
    }
    return resultResponse(creatures::ws::PlaylistService::getPlaylist(playlistId), span,
                          [](const auto &playlist) { return playlistToJson(playlist); });
}

PreparedResponse upsertPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    return resultResponse(creatures::ws::PlaylistService::upsertPlaylist(body), span,
                          [](const auto &playlist) { return playlistToJson(playlist); });
}

PreparedResponse startPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    auto parsing = parseSpan("PlaylistController.parseStartRequest", span);
    const auto jsonResult = JsonParser::parseApiJsonString(body, "playlist start request", parsing);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), span);
    }
    const auto parsedResult = api::startPlaylistRequestFromJson(jsonResult.getValue().value());
    if (!parsedResult.isSuccess()) {
        return errorResponse(parsedResult.getError().value(), span);
    }
    if (parsing) {
        parsing->setSuccess();
    }
    const auto parsed = parsedResult.getValue().value();
    if (span) {
        span->setAttribute("playlist.id", parsed.playlistId);
        span->setAttribute("playlist.universe", static_cast<int64_t>(parsed.universe));
    }
    return resultResponse(creatures::ws::PlaylistService::startPlaylist(parsed.universe, parsed.playlistId), span,
                          [](const auto &status) { return api::statusResponseToJson(status); });
}

PreparedResponse stopPlaylist(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    auto parsing = parseSpan("PlaylistController.parseStopRequest", span);
    const auto jsonResult = JsonParser::parseApiJsonString(body, "playlist stop request", parsing);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), span);
    }
    const auto parsedResult = api::stopPlaylistRequestFromJson(jsonResult.getValue().value());
    if (!parsedResult.isSuccess()) {
        return errorResponse(parsedResult.getError().value(), span);
    }
    if (parsing) {
        parsing->setSuccess();
    }
    const auto universe = parsedResult.getValue()->universe;
    if (span) {
        span->setAttribute("playlist.universe", static_cast<int64_t>(universe));
    }
    return resultResponse(creatures::ws::PlaylistService::stopPlaylist(universe), span,
                          [](const auto &status) { return api::statusResponseToJson(status); });
}

PreparedResponse getPlaylistStatus(const std::string &universeText, const std::shared_ptr<OperationSpan> &span) {
    universe_t universe = 0;
    const auto [end, error] = std::from_chars(universeText.data(), universeText.data() + universeText.size(), universe);
    if (error != std::errc{} || end != universeText.data() + universeText.size() ||
        universe < api::MIN_PLAYLIST_UNIVERSE || universe > api::MAX_PLAYLIST_UNIVERSE) {
        return errorResponse(ServerError(ServerError::InvalidData, "universe must be between 1 and 63999"), span);
    }
    if (span) {
        span->setAttribute("playlist.universe", static_cast<int64_t>(universe));
    }
    return resultResponse(creatures::ws::PlaylistService::playlistStatus(universe), span,
                          [](const auto &status) { return playlistStatusToJson(status); });
}

PreparedResponse listPlaylistStatuses(const std::shared_ptr<OperationSpan> &span) {
    return resultResponse(creatures::ws::PlaylistService::getAllPlaylistStatuses(), span,
                          [](const auto &statuses) { return api::listResponseToJson(statuses, playlistStatusToJson); });
}

} // namespace creatures::transport
