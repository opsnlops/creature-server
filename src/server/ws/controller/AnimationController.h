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
#include "util/ObservabilityManager.h"
#include "util/websocketUtils.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObservabilityManager> observability; // Add this global
}

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures :: ws {

    class AnimationController : public oatpp::web::server::api::ApiController {
    public:
        AnimationController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        AnimationService m_animationService;
    public:

        static std::shared_ptr<AnimationController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper)
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
            // 🐰 Create a trace span for this request
            auto span = creatures::observability->createRequestSpan(
                "GET /api/v1/animation", "GET", "api/v1/animation"
            );

            debug("REST call to listAllAnimations");
            creatures::metrics->incrementRestRequestsProcessed();

            if (span) {
                span->setAttribute("endpoint", "listAllAnimations");
                span->setAttribute("controller", "AnimationController");
            }

            auto result = m_animationService.listAllAnimations();

            // Record success metrics in the span
            if (span) {
                span->setAttribute("animations.count", static_cast<int64_t>(result->count));
                span->setHttpStatus(200);
            }

            return createDtoResponse(Status::CODE_200, result);
        }

        ENDPOINT_INFO(getAnimation) {
            info->summary = "Get an animation by id";
            info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
            info->pathParams["animationId"].description = "Animation ID in the form of a MongoDB OID";
        }
        ENDPOINT("GET", "api/v1/animation/{animationId}", getAnimation,
                 PATH(String, animationId))
        {
            // 🥕 RequestSpan only handles HTTP-level concerns
            auto span = creatures::observability->createRequestSpan(
                "GET /api/v1/animation/{animationId}", "GET",
                "api/v1/animation/" + std::string(animationId)
            );

            debug("get animation by ID via REST API: {}", std::string(animationId));
            creatures::metrics->incrementRestRequestsProcessed();

            if (span) {
                span->setAttribute("endpoint", "getAnimation");
                span->setAttribute("controller", "AnimationController");
                span->setAttribute("animation.id", std::string(animationId));
            }

            // The service call will create its own OperationSpan
            auto result = m_animationService.getAnimation(animationId);

            if (span) {
                // HTTP handler just cares about HTTP success
                span->setAttribute("animation.title", std::string(result->metadata->title));
                span->setHttpStatus(200);
            }

            return createDtoResponse(Status::CODE_200, result);
        }

        ENDPOINT_INFO(upsertAnimation) {
            info->summary = "Create or update an animation in the database";
            info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/animation", upsertAnimation,
                 REQUEST(std::shared_ptr<IncomingRequest>, request))
        {
            // 🌟 Span for the upsert operation
            auto span = creatures::observability->createRequestSpan(
                "POST /api/v1/animation", "POST", "api/v1/animation"
            );

            debug("new animation uploaded via REST API");
            creatures::metrics->incrementRestRequestsProcessed();

            try {
                auto requestAsString = std::string(request->readBodyToString());
                trace("request was: {}", requestAsString);

                if (span) {
                    span->setAttribute("endpoint", "upsertAnimation");
                    span->setAttribute("controller", "AnimationController");
                    span->setAttribute("request.body_size", static_cast<int64_t>(requestAsString.length()));
                }

                auto result = m_animationService.upsertAnimation(requestAsString);

                if (span) {
                    span->setAttribute("animation.id", std::string(result->id));
                    span->setAttribute("animation.title", std::string(result->metadata->title));
                    span->setHttpStatus(200);
                }

                // Schedule cache invalidation (this could be traced too!)
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);

                return createDtoResponse(Status::CODE_200, result);

            } catch (const std::exception& ex) {
                if (span) {
                    span->recordException(ex);
                    span->setHttpStatus(500);
                }

                error("Exception in upsertAnimation: {}", ex.what());
                throw;
            }
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
            // 🎭 The most exciting span - playing an animation!
            auto span = creatures::observability->createRequestSpan(
                "POST /api/v1/animation/play", "POST", "api/v1/animation/play"
            );

            creatures::metrics->incrementRestRequestsProcessed();

            try {
                if (span) {
                    span->setAttribute("endpoint", "playStoredAnimation");
                    span->setAttribute("controller", "AnimationController");
                    span->setAttribute("animation.id", std::string(requestBody->animation_id));
                    span->setAttribute("universe", static_cast<int64_t>(requestBody->universe));
                }

                auto result = m_animationService.playStoredAnimation(
                    std::string(requestBody->animation_id),
                    requestBody->universe
                );

                if (span) {
                    span->setAttribute("result.message", std::string(result->message));
                    span->setHttpStatus(200);
                }

                return createDtoResponse(Status::CODE_200, result);

            } catch (const std::exception& ex) {
                if (span) {
                    span->recordException(ex);
                    span->setHttpStatus(500);
                }

                error("Exception in playStoredAnimation: {}", ex.what());
                throw;
            }
        }
    };
}

#include OATPP_CODEGEN_END(ApiController)