
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include "model/Playlist.h"

#include "server/database.h"

#include "model/PlaylistStatus.h"
#include "server/ws/dto/StartPlaylistRequestDto.h"
#include "server/ws/dto/StopPlaylistRequestDto.h"
#include "server/ws/service/PlaylistService.h"
#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class PlaylistController : public oatpp::web::server::api::ApiController {
    public:
        PlaylistController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        PlaylistService m_playlistService; // Create the animation service
    public:

        static std::shared_ptr<PlaylistController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper) // Inject objectMapper component here as default parameter
        ) {
            return std::make_shared<PlaylistController>(objectMapper);
        }

        ENDPOINT_INFO(getAllPlaylists) {
            info->summary = "Get all of the playlists";

            info->addResponse<Object<AnimationsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/playlist", getAllPlaylists)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_playlistService.getAllPlaylists());
        }

        ENDPOINT_INFO(getPlaylist) {
            info->summary = "Get a playlist by id";

            info->addResponse<Object<PlaylistDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");

            info->pathParams["playlistId"].description = "Playlist ID in the form of a UUID";
        }
        ENDPOINT("GET", "api/v1/playlist/id/{playlistId}", getPlaylist,
                 PATH(String, playlistId))
        {
            debug("get playlist by ID via REST API: {}", std::string(playlistId));
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_playlistService.getPlaylist(playlistId));
        }

        /**
         * This one is like the Creature upsert. It allows any JSON to come in. It validates that the
         * JSON is correct, but stores whatever comes in in the DB.
         *
         * @return
         */
        ENDPOINT_INFO(upsertPlaylist) {
            info->summary = "Create or update a playlist in the database";

            info->addResponse<Object<PlaylistDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/playlist", upsertPlaylist,
                 REQUEST(std::shared_ptr<IncomingRequest>, request))
        {
            debug("new playlist uploaded via REST API");
            creatures::metrics->incrementRestRequestsProcessed();
            auto requestAsString = std::string(request->readBodyToString());
            trace("request was: {}", requestAsString);

            // Schedule an event to invalidate the playlist cache on the clients
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Playlist);

            return createDtoResponse(Status::CODE_200,
                                     m_playlistService.upsertPlaylist(requestAsString));
        }

        ENDPOINT_INFO(startPlaylist) {
            info->summary = "Start a playlist";

            info->addResponse<Object<PlaylistStatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/playlist/start", startPlaylist,
                 BODY_DTO(Object<StartPlaylistRequestDto>, playlistStartDto))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200,
                                     m_playlistService.startPlaylist(playlistStartDto->universe, playlistStartDto->playlist_id));
        }

        ENDPOINT_INFO(stopPlaylist) {
            info->summary = "Stop a playlist";

            info->addResponse<Object<PlaylistStatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/playlist/stop", stopPlaylist,
                 BODY_DTO(Object<StopPlaylistRequestDto>, stopPlaylistDto))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200,
                                     m_playlistService.stopPlaylist(stopPlaylistDto->universe));
        }

        // Get the status a universe's playlist
        ENDPOINT_INFO(playlistStatus) {
            info->summary = "Get the status of a universe's playlist";

            info->addResponse<Object<PlaylistStatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/playlist/status/{universe}", playlistStatus,
                 PATH(UInt32, universe))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_playlistService.playlistStatus(universe));
        }

        // Get the status of all playlists
        ENDPOINT_INFO(getAllPlaylistStatuses) {
            info->summary = "Get the status of all playlists";

            info->addResponse<Object<ListDto<oatpp::Object<PlaylistStatusDto>>>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/playlist/status", getAllPlaylistStatuses)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_playlistService.getAllPlaylistStatuses());
        }
    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen