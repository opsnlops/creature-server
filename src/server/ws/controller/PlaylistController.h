#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/JsonResponse.h"
#include "api/PlaylistRequests.h"
#include "model/Playlist.h"
#include "model/PlaylistStatus.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/service/PlaylistService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

class PlaylistController : public oatpp::web::server::api::ApiController,
                           public HttpResponseHelpers<PlaylistController> {
  public:
    PlaylistController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    static std::shared_ptr<PlaylistController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                            objectMapper)) {
        return std::make_shared<PlaylistController>(objectMapper);
    }

    ENDPOINT_INFO(getAllPlaylists) {
        info->summary = "Get all playlists";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist", getAllPlaylists, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/playlist", "GET", "api/v1/playlist", "getAllPlaylists", "PlaylistController",
                           request, [&](const auto &span) {
                               const auto result = PlaylistService::getAllPlaylists(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::listResponseToJson(result.getValue().value(), playlistToJson));
                           });
    }

    ENDPOINT_INFO(getPlaylist) {
        info->summary = "Get a playlist by ID";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["playlistId"].description = "Playlist ID in UUID form";
    }
    ENDPOINT("GET", "api/v1/playlist/id/{playlistId}", getPlaylist, PATH(String, playlistId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/playlist/id/{playlistId}", "GET",
                           "api/v1/playlist/id/" + std::string(playlistId), "getPlaylist", "PlaylistController",
                           request, [&](const auto &span) {
                               if (!playlistId || !isUuidShape(std::string(playlistId)))
                                   return bailHttp(span, Status::CODE_400, "playlistId must be a UUID");
                               if (span)
                                   span->setAttribute("playlist.id", std::string(playlistId));
                               const auto result = PlaylistService::getPlaylist(std::string(playlistId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200, playlistToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(upsertPlaylist) {
        info->summary = "Create or update a playlist";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_413, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist", upsertPlaylist, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/playlist", "POST", "api/v1/playlist", "upsertPlaylist", "PlaylistController",
                           request, [&](const auto &span) {
                               const auto body = readRequestBodyLimited(request, MAX_PLAYLIST_REQUEST_BODY_BYTES, span);
                               const auto result = PlaylistService::upsertPlaylist(body, span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto playlist = result.getValue().value();
                               if (span) {
                                   span->setAttribute("playlist.id", playlist.id);
                                   span->setAttribute("playlist.name", playlist.name);
                                   span->setAttribute("playlist.items", static_cast<int64_t>(playlist.number_of_items));
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, playlistToJson(playlist));
                           });
    }

    ENDPOINT_INFO(startPlaylist) {
        info->summary = "Start a playlist";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist/start", startPlaylist, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/playlist/start", "POST", "api/v1/playlist/start", "startPlaylist", "PlaylistController",
            request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_PLAYLIST_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan =
                    observability
                        ? observability->createChildOperationSpan("PlaylistController.parseStartRequest", span)
                        : nullptr;
                const auto jsonResult = JsonParser::parseApiJsonString(body, "playlist start request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto requestResult = api::startPlaylistRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidPlaylistStartRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto parsed = requestResult.getValue().value();
                if (span) {
                    span->setAttribute("playlist.id", parsed.playlistId);
                    span->setAttribute("playlist.universe", static_cast<int64_t>(parsed.universe));
                }
                const auto result = PlaylistService::startPlaylist(parsed.universe, parsed.playlistId, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::statusResponseToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(stopPlaylist) {
        info->summary = "Stop a playlist";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist/stop", stopPlaylist, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/playlist/stop", "POST", "api/v1/playlist/stop", "stopPlaylist", "PlaylistController", request,
            [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_PLAYLIST_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan =
                    observability ? observability->createChildOperationSpan("PlaylistController.parseStopRequest", span)
                                  : nullptr;
                const auto jsonResult = JsonParser::parseApiJsonString(body, "playlist stop request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto requestResult = api::stopPlaylistRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidPlaylistStopRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto universe = requestResult.getValue()->universe;
                if (span)
                    span->setAttribute("playlist.universe", static_cast<int64_t>(universe));
                const auto result = PlaylistService::stopPlaylist(universe, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::statusResponseToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(playlistStatus) {
        info->summary = "Get a universe's playlist status";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist/status/{universe}", playlistStatus, PATH(UInt32, universe),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/playlist/status/{universe}", "GET", "api/v1/playlist/status/" + std::to_string(*universe),
            "playlistStatus", "PlaylistController", request, [&](const auto &span) {
                if (!universe || *universe < api::MIN_PLAYLIST_UNIVERSE || *universe > api::MAX_PLAYLIST_UNIVERSE)
                    return bailHttp(span, Status::CODE_400, "universe must be between 1 and 63999");
                if (span)
                    span->setAttribute("playlist.universe", static_cast<int64_t>(*universe));
                const auto result = PlaylistService::playlistStatus(*universe, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, playlistStatusToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(getAllPlaylistStatuses) {
        info->summary = "Get all playlist statuses";
        info->addTag("Playlists");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist/status", getAllPlaylistStatuses,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/playlist/status", "GET", "api/v1/playlist/status", "getAllPlaylistStatuses",
                           "PlaylistController", request, [&](const auto &span) {
                               const auto result = PlaylistService::getAllPlaylistStatuses(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(
                                   span, Status::CODE_200,
                                   api::listResponseToJson(result.getValue().value(), playlistStatusToJson));
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
