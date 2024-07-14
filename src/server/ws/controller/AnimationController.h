
#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>


#include "model/AnimationMetadata.h"

#include "server/config.h"
#include "server/database.h"
#include "server/ws/service/AnimationService.h"
#include "server/ws/dto/PlayAnimationRequestDto.h"
#include "server/metrics/counters.h"
#include "util/websocketUtils.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

namespace creatures :: ws {

    class AnimationController : public oatpp::web::server::api::ApiController {
    public:
        AnimationController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        AnimationService m_animationService; // Create the animation service
    public:

        static std::shared_ptr<AnimationController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper) // Inject objectMapper component here as default parameter
        ) {
            return std::make_shared<AnimationController>(objectMapper);
        }

        ENDPOINT_INFO(listAllAnimations) {
            info->summary = "List all of the animations";

            info->addResponse<Object<AnimationsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("GET", "api/v1/animation", listAllAnimations)
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_animationService.listAllAnimations());
        }

        ENDPOINT_INFO(getAnimation) {
            info->summary = "Get an animation by id";

            info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");

            info->pathParams["creatureId"].description = "Animation ID in the form of a MongoDB OID";
        }
        ENDPOINT("GET", "api/v1/animation/{animationId}", getAnimation,
                 PATH(String, animationId))
        {
            debug("get animation by ID via REST API: {}", std::string(animationId));
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200, m_animationService.getAnimation(animationId));
        }

        /**
         * This one is like the Creature upsert. It allows any JSON to come in. It validates that the
         * JSON is correct, but stores whatever comes in in the DB.
         *
         * @return
         */
        ENDPOINT_INFO(upsertAnimation) {
            info->summary = "Create or update an animation in the database";

            info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/animation", upsertAnimation,
                 REQUEST(std::shared_ptr<IncomingRequest>, request))
        {
            debug("new animation uploaded via REST API");
            creatures::metrics->incrementRestRequestsProcessed();
            auto requestAsString = std::string(request->readBodyToString());
            trace("request was: {}", requestAsString);

            // Schedule an event to invalidate the animation cache on the clients
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);

            return createDtoResponse(Status::CODE_200,
                                     m_animationService.upsertAnimation(requestAsString));
        }


        ENDPOINT_INFO(playStoredAnimation) {
            info->summary = "Play one animation out of the database on a given universe";

            info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/animation/play", playStoredAnimation,
                 BODY_DTO(Object<creatures::ws::PlayAnimationRequestDto>, requestBody))
        {
            creatures::metrics->incrementRestRequestsProcessed();
            return createDtoResponse(Status::CODE_200,
                                     m_animationService.playStoredAnimation(std::string(requestBody->animation_id),
                                                                            requestBody->universe));
        }

    };

}

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen