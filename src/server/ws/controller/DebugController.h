
#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "server/config.h"
#include "server/metrics/counters.h"
#include "server/ws/dto/StatusDto.h"

#include "server/metrics/counters.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures ::ws {

class DebugController : public oatpp::web::server::api::ApiController {
  public:
    DebugController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    // MetricsService m_metricsService;
  public:
    static std::shared_ptr<DebugController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<DebugController>(objectMapper);
    }

    ENDPOINT_INFO(invalidate_creature) {
        info->summary = "Sends a message to all clients to invalidate their creature cache";

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/creature", invalidate_creature) {
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Creature);

        auto statusMessage =
            fmt::format("Creature cache invalidation scheduled for {} frames from now", CACHE_INVALIDATION_DELAY_TIME);
        debug(statusMessage);

        auto status = StatusDto::createShared();
        status->code = 200;
        status->message = statusMessage;
        status->status = "OK";
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, status);
    }

    ENDPOINT_INFO(invalidate_animation) {
        info->summary = "Sends a message to all clients to invalidate their animation cache";

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/animation", invalidate_animation) {

        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);

        auto statusMessage =
            fmt::format("Animation cache invalidation scheduled for {} frames from now", CACHE_INVALIDATION_DELAY_TIME);
        debug(statusMessage);

        auto status = StatusDto::createShared();
        status->code = 200;
        status->message = statusMessage;
        status->status = "OK";
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, status);
    }

    ENDPOINT_INFO(invalidate_playlist) {
        info->summary = "Sends a message to all clients to invalidate their playlist cache";

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/cache-invalidate/playlist", invalidate_playlist) {

        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Playlist);

        auto statusMessage =
            fmt::format("Playlist cache invalidation scheduled for {} frames from now", CACHE_INVALIDATION_DELAY_TIME);
        debug(statusMessage);

        auto status = StatusDto::createShared();
        status->code = 200;
        status->message = statusMessage;
        status->status = "OK";
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, status);
    }

    ENDPOINT_INFO(test_playlist_updates) {
        info->summary = "Tests the ability to send a playlist update";

        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/debug/playlist/update", test_playlist_updates) {

        PlaylistStatus playlistStatus{};
        playlistStatus.universe = 42;
        playlistStatus.playlist = "4b5aa09e-9a61-47e7-86d2-3d8f59ebd9a7";
        playlistStatus.playing = true;
        playlistStatus.current_animation = "2241e872-57b3-4fa3-8e76-1c2f517f998d";

        broadcastPlaylistStatusToAllClients(playlistStatus);

        auto status = StatusDto::createShared();
        status->code = 200;
        status->message = "Playlist update sent";
        status->status = "OK";
        creatures::metrics->incrementRestRequestsProcessed();
        return createDtoResponse(Status::CODE_200, status);
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen