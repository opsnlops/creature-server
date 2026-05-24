
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/Playlist.h"

#include "server/database.h"

#include "model/PlaylistStatus.h"
#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/StartPlaylistRequestDto.h"
#include "server/ws/dto/StopPlaylistRequestDto.h"
#include "server/ws/service/PlaylistService.h"

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures ::ws {

class PlaylistController : public oatpp::web::server::api::ApiController {
  public:
    PlaylistController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    PlaylistService m_playlistService; // Create the animation service
  public:
    static std::shared_ptr<PlaylistController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<PlaylistController>(objectMapper);
    }

    ENDPOINT_INFO(getAllPlaylists) {
        info->summary = "Get all of the playlists";
        info->addTag("Playlists");

        info->addResponse<Object<AnimationsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist", getAllPlaylists, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("REST request to get all playlists");
        return runEndpoint("GET /api/v1/playlist", "GET", "api/v1/playlist", "getAllPlaylists", "PlaylistController",
                           request, [&](const auto &span) {
                               const auto result = m_playlistService.getAllPlaylists();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(getPlaylist) {
        info->summary = "Get a playlist by id";
        info->addTag("Playlists");

        info->addResponse<Object<PlaylistDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");

        info->pathParams["playlistId"].description = "Playlist ID in the form of a UUID";
    }
    ENDPOINT("GET", "api/v1/playlist/id/{playlistId}", getPlaylist, PATH(String, playlistId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("get playlist by ID via REST API: {}", std::string(playlistId));
        return runEndpoint("GET /api/v1/playlist/id/{playlistId}", "GET",
                           "api/v1/playlist/id/" + std::string(playlistId), "getPlaylist", "PlaylistController",
                           request, [&](const auto &span) {
                               OATPP_ASSERT_HTTP(playlistId && isUuidShape(std::string(playlistId)), Status::CODE_400,
                                                 "playlistId must be a UUID");
                               if (span)
                                   span->setAttribute("playlist.id", std::string(playlistId));
                               const auto result = m_playlistService.getPlaylist(playlistId);
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    /**
     * This one is like the Creature upsert. It allows any JSON to come in. It validates that the
     * JSON is correct, but stores whatever comes in in the DB.
     *
     * @return
     */
    ENDPOINT_INFO(upsertPlaylist) {
        info->summary = "Create or update a playlist in the database";
        info->addTag("Playlists");

        info->addResponse<Object<PlaylistDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist", upsertPlaylist, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("new playlist uploaded via REST API");
        return runEndpoint("POST /api/v1/playlist", "POST", "api/v1/playlist", "upsertPlaylist", "PlaylistController",
                           request, [&](const auto &span) {
                               auto requestAsString = std::string(request->readBodyToString());
                               trace("request was: {}", requestAsString);
                               if (span)
                                   span->setAttribute("request.body_size",
                                                      static_cast<int64_t>(requestAsString.size()));

                               // Schedule an event to invalidate the playlist cache on the clients
                               scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Playlist);

                               const auto result = m_playlistService.upsertPlaylist(requestAsString);
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(startPlaylist) {
        info->summary = "Start a playlist";
        info->addTag("Playlists");

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist/start", startPlaylist,
             BODY_DTO(Object<StartPlaylistRequestDto>, playlistStartDto),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/playlist/start", "POST", "api/v1/playlist/start", "startPlaylist", "PlaylistController",
            request, [&](const auto &span) {
                if (span && playlistStartDto) {
                    if (playlistStartDto->universe) {
                        span->setAttribute("playlist.universe", static_cast<int64_t>(*playlistStartDto->universe));
                    }
                    if (playlistStartDto->playlist_id) {
                        span->setAttribute("playlist.id", std::string(playlistStartDto->playlist_id));
                    }
                }
                const auto result =
                    m_playlistService.startPlaylist(playlistStartDto->universe, playlistStartDto->playlist_id);
                if (span)
                    span->setHttpStatus(200);
                return createDtoResponse(Status::CODE_200, result);
            });
    }

    ENDPOINT_INFO(stopPlaylist) {
        info->summary = "Stop a playlist";
        info->addTag("Playlists");

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/playlist/stop", stopPlaylist, BODY_DTO(Object<StopPlaylistRequestDto>, stopPlaylistDto),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/playlist/stop", "POST", "api/v1/playlist/stop", "stopPlaylist",
                           "PlaylistController", request, [&](const auto &span) {
                               if (span && stopPlaylistDto && stopPlaylistDto->universe) {
                                   span->setAttribute("playlist.universe",
                                                      static_cast<int64_t>(*stopPlaylistDto->universe));
                               }
                               const auto result = m_playlistService.stopPlaylist(stopPlaylistDto->universe);
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    // Get the status a universe's playlist
    ENDPOINT_INFO(playlistStatus) {
        info->summary = "Get the status of a universe's playlist";
        info->addTag("Playlists");

        info->addResponse<Object<PlaylistStatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist/status/{universe}", playlistStatus, PATH(UInt32, universe),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/playlist/status/{universe}", "GET",
                           "api/v1/playlist/status/" + std::to_string(*universe), "playlistStatus",
                           "PlaylistController", request, [&](const auto &span) {
                               if (span && universe) {
                                   span->setAttribute("playlist.universe", static_cast<int64_t>(*universe));
                               }
                               const auto result = m_playlistService.playlistStatus(universe);
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    // Get the status of all playlists
    ENDPOINT_INFO(getAllPlaylistStatuses) {
        info->summary = "Get the status of all playlists";
        info->addTag("Playlists");

        info->addResponse<Object<ListDto<oatpp::Object<PlaylistStatusDto>>>>(Status::CODE_200,
                                                                             "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/playlist/status", getAllPlaylistStatuses,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/playlist/status", "GET", "api/v1/playlist/status", "getAllPlaylistStatuses",
                           "PlaylistController", request, [&](const auto &span) {
                               const auto result = m_playlistService.getAllPlaylistStatuses();
                               if (span)
                                   span->setHttpStatus(200);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen