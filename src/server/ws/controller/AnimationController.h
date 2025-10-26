#pragma once

#include <fmt/format.h>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/AnimationMetadata.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/metrics/counters.h"
#include "server/ws/dto/AdHocAnimationDto.h"
#include "server/ws/dto/CreateAdHocAnimationRequestDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/PlayAnimationRequestDto.h"
#include "server/ws/service/AnimationService.h"
#include "util/ObservabilityManager.h"
#include "util/websocketUtils.h"
#include <nlohmann/json.hpp>

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<Database> db;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class AnimationController : public oatpp::web::server::api::ApiController {
  public:
    explicit AnimationController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    AnimationService m_animationService;

  public:
    static std::shared_ptr<AnimationController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                             objectMapper)) {
        return std::make_shared<AnimationController>(objectMapper);
    }

    // Helper function to add common HTTP attributes to a span
    void addHttpRequestAttributes(const std::shared_ptr<creatures::RequestSpan> &span,
                                  const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
        if (span && request) {
            span->setAttribute("http.method", std::string(request->getStartingLine().method.toString()));
            span->setAttribute("http.target", std::string(request->getStartingLine().path.toString()));

            // Add User-Agent if present (getHeader works directly)
            auto userAgent = request->getHeader("User-Agent");
            if (userAgent) {
                span->setAttribute("http.user_agent", std::string(userAgent));
            }

            // Add Content-Length if present
            auto contentLength = request->getHeader("Content-Length");
            if (contentLength) {
                span->setAttribute("http.request_content_length", std::string(contentLength));
            }

            // Add Host if present
            auto host = request->getHeader("Host");
            if (host) {
                span->setAttribute("http.host", std::string(host));
            }
            span->setAttribute("http.flavor", "1.1"); // Assuming HTTP/1.1 for oatpp
        }
    }

    ENDPOINT_INFO(listAllAnimations) {
        info->summary = "List all of the animations";
        info->addResponse<Object<AnimationsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation", listAllAnimations,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // Create a trace span for this request
        auto span = creatures::observability->createRequestSpan("GET /api/v1/animation", "GET", "api/v1/animation");
        addHttpRequestAttributes(span, request); // Add HTTP attributes

        debug("REST call to listAllAnimations");
        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "listAllAnimations");
            span->setAttribute("controller", "AnimationController");
        }

        auto result = m_animationService.listAllAnimations(std::move(span));

        // Record success metrics in the span
        if (span) {
            span->setAttribute("animations.count", static_cast<int64_t>(result->count));
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(listAdHocAnimations) {
        info->summary = "List ad-hoc animations stored in the TTL collection";
        info->addResponse<Object<AdHocAnimationListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc", listAdHocAnimations,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span =
            creatures::observability->createRequestSpan("GET /api/v1/animation/ad-hoc", "GET", "api/v1/animation/ad-hoc");
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "listAdHocAnimations");
            span->setAttribute("controller", "AnimationController");
        }

        auto result = m_animationService.listAdHocAnimations(span);
        if (span) {
            span->setHttpStatus(200);
            span->setAttribute("adhoc.count", static_cast<int64_t>(result->count));
        }
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(getAdHocAnimation) {
        info->summary = "Get an ad-hoc animation by id";
        info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Ad-hoc animation ID";
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc/{animationId}", getAdHocAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span = creatures::observability->createRequestSpan("GET /api/v1/animation/ad-hoc/{animationId}", "GET",
                                                                "api/v1/animation/ad-hoc/" + std::string(animationId));
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "getAdHocAnimation");
            span->setAttribute("controller", "AnimationController");
            span->setAttribute("animation.id", std::string(animationId));
        }

        auto result = m_animationService.getAdHocAnimation(animationId, span);

        if (span) {
            span->setAttribute("animation.title", std::string(result->metadata->title));
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
    ENDPOINT("GET", "api/v1/animation/{animationId}", getAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // RequestSpan only handles HTTP-level concerns
        auto span = creatures::observability->createRequestSpan("GET /api/v1/animation/{animationId}", "GET",
                                                                "api/v1/animation/" + std::string(animationId));
        addHttpRequestAttributes(span, request); // Add HTTP attributes

        debug("get animation by ID via REST API: {}", std::string(animationId));
        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "getAnimation");
            span->setAttribute("controller", "AnimationController");
            span->setAttribute("animation.id", std::string(animationId));
        }

        // The service call will create its own OperationSpan
        auto result = m_animationService.getAnimation(animationId, span);

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
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // Span for the upsert operation
        auto span = creatures::observability->createRequestSpan("POST /api/v1/animation", "POST", "api/v1/animation");
        addHttpRequestAttributes(span, request); // Add HTTP attributes

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

            auto result = m_animationService.upsertAnimation(requestAsString, span);

            if (span) {
                span->setAttribute("animation.id", std::string(result->id));
                span->setAttribute("animation.title", std::string(result->metadata->title));
                span->setHttpStatus(200);
            }

            // Schedule cache invalidation (this could be traced too!)
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);

            return createDtoResponse(Status::CODE_200, result);

        } catch (const std::exception &ex) {
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
             BODY_DTO(Object<creatures::ws::PlayAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // The most exciting span - playing an animation!
        auto span =
            creatures::observability->createRequestSpan("POST /api/v1/animation/play", "POST", "api/v1/animation/play");
        addHttpRequestAttributes(span, request); // Add HTTP attributes

        creatures::metrics->incrementRestRequestsProcessed();

        try {
            if (span) {
                span->setAttribute("endpoint", "playStoredAnimation");
                span->setAttribute("controller", "AnimationController");
                span->setAttribute("animation.id", std::string(requestBody->animation_id));
                span->setAttribute("universe", static_cast<int64_t>(requestBody->universe));
            }

            auto result =
                m_animationService.playStoredAnimation(std::string(requestBody->animation_id), requestBody->universe);

            if (span) {
                span->setAttribute("result.message", std::string(result->message));
                span->setHttpStatus(200);
            }

            return createDtoResponse(Status::CODE_200, result);

        } catch (const std::exception &ex) {
            if (span) {
                span->recordException(ex);
                span->setHttpStatus(500);
            }

            error("Exception in playStoredAnimation: {}", ex.what());
            throw;
        }
    }

    ENDPOINT_INFO(interruptAnimation) {
        info->summary = "Interrupt current playback with a new animation (for interactive Zoom meetings!)";
        info->description =
            "Requires cooperative scheduler (--scheduler cooperative). Returns 400 if legacy scheduler is active.";
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/interrupt", interruptAnimation,
             BODY_DTO(Object<creatures::ws::PlayAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span = creatures::observability->createRequestSpan("POST /api/v1/animation/interrupt", "POST",
                                                                "api/v1/animation/interrupt");
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        // Check if cooperative scheduler is enabled
        if (creatures::config->getAnimationSchedulerType() !=
            creatures::Configuration::AnimationSchedulerType::Cooperative) {
            auto result = creatures::ws::StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message =
                "Animation interrupts require the cooperative scheduler. Start server with --scheduler cooperative";

            if (span) {
                span->setAttribute("error.type", "scheduler_not_supported");
                span->setAttribute("error.message", std::string(result->message));
                span->setAttribute("scheduler_type", "legacy");
                span->setHttpStatus(400);
            }

            error("Interrupt API called with legacy scheduler enabled");
            return createDtoResponse(Status::CODE_400, result);
        }

        try {
            if (span) {
                span->setAttribute("endpoint", "interruptAnimation");
                span->setAttribute("controller", "AnimationController");
                span->setAttribute("animation.id", std::string(requestBody->animation_id));
                span->setAttribute("universe", static_cast<int64_t>(requestBody->universe));
                span->setAttribute("resume_playlist", static_cast<bool>(requestBody->resumePlaylist));
            }

            bool shouldResume = requestBody->resumePlaylist ? true : false;
            info("REST API: interrupting universe {} with animation {} (resume: {})", requestBody->universe,
                 std::string(requestBody->animation_id), shouldResume);

            // Get the animation from the database
            auto animationDto = m_animationService.getAnimation(requestBody->animation_id, span);

            // Convert from oatpp DTO to internal model
            std::shared_ptr<AnimationDto> animationDtoPtr(animationDto.get(),
                                                          [](AnimationDto *) {}); // Non-owning shared_ptr
            auto animation = convertFromDto(animationDtoPtr);

            // Use SessionManager to interrupt
            auto sessionResult = creatures::sessionManager->interrupt(requestBody->universe, animation, shouldResume);

            if (!sessionResult.isSuccess()) {
                auto errorMsg = sessionResult.getError()->getMessage();
                error("Failed to interrupt animation: {}", errorMsg);

                if (span) {
                    span->setAttribute("error.message", errorMsg);
                    span->setHttpStatus(500);
                }

                auto result = creatures::ws::StatusDto::createShared();
                result->status = "error";
                result->code = 500;
                result->message = errorMsg.c_str();

                return createDtoResponse(Status::CODE_500, result);
            }

            // Success!
            if (span) {
                span->setAttribute("result.success", true);
                span->setHttpStatus(200);
            }

            auto result = creatures::ws::StatusDto::createShared();
            result->status = "success";
            result->code = 200;
            result->message = "Animation interrupt scheduled successfully";

            return createDtoResponse(Status::CODE_200, result);

        } catch (const std::exception &ex) {
            if (span) {
                span->recordException(ex);
                span->setHttpStatus(500);
            }

            error("Exception in interruptAnimation: {}", ex.what());
            throw;
        }
    }

    ENDPOINT_INFO(createAdHocAnimation) {
        info->summary = "Generate and play an ad-hoc speech animation";
        info->description =
            "Creates a job that synthesizes audio, generates lip sync, stores a temporary animation, and interrupts.";
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc", createAdHocAnimation,
             BODY_DTO(Object<creatures::ws::CreateAdHocAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span = creatures::observability->createRequestSpan("POST /api/v1/animation/ad-hoc", "POST",
                                                                "api/v1/animation/ad-hoc");
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        if (creatures::config->getAnimationSchedulerType() !=
            creatures::Configuration::AnimationSchedulerType::Cooperative) {
            auto result = creatures::ws::StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "Ad-hoc speech requires the cooperative scheduler (--scheduler cooperative)";
            if (span) {
                span->setAttribute("error.type", "scheduler_not_supported");
                span->setHttpStatus(400);
            }
            return createDtoResponse(Status::CODE_400, result);
        }

        auto creatureId = requestBody->creature_id ? std::string(requestBody->creature_id) : "";
        auto text = requestBody->text ? std::string(requestBody->text) : "";
        bool resumePlaylist = requestBody->resume_playlist ? static_cast<bool>(requestBody->resume_playlist) : true;

        if (span) {
            span->setAttribute("creature.id", creatureId);
            span->setAttribute("text.length", static_cast<int64_t>(text.size()));
            span->setAttribute("resume_playlist", resumePlaylist);
        }

        if (creatureId.empty() || text.empty()) {
            auto result = creatures::ws::StatusDto::createShared();
            result->status = "error";
            result->code = 400;
            result->message = "creature_id and text are required";
            if (span) {
                span->setAttribute("error.type", "invalid_request");
                span->setHttpStatus(400);
            }
            return createDtoResponse(Status::CODE_400, result);
        }

        auto creatureLookupSpan =
            creatures::observability->createChildOperationSpan("AnimationController.lookupCreature", nullptr);
        if (creatureLookupSpan) {
            creatureLookupSpan->setAttribute("creature.id", creatureId);
        }

        auto creatureResult = creatures::db->getCreature(creatureId, creatureLookupSpan);
        if (!creatureResult.isSuccess()) {
            auto error = creatureResult.getError().value();
            auto result = creatures::ws::StatusDto::createShared();
            result->status = "error";
            result->message = error.getMessage().c_str();
            Status status = Status::CODE_500;
            switch (error.getCode()) {
            case ServerError::NotFound:
                status = Status::CODE_404;
                break;
            case ServerError::InvalidData:
                status = Status::CODE_400;
                break;
            default:
                status = Status::CODE_500;
                break;
            }
            result->code = status.code;
            if (creatureLookupSpan) {
                creatureLookupSpan->setError(error.getMessage());
                creatureLookupSpan->setAttribute("error.code", static_cast<int64_t>(status.code));
            }
            if (span) {
                span->setAttribute("error.type", "creature_lookup_failed");
                span->setHttpStatus(status.code);
            }
            return createDtoResponse(status, result);
        }

        if (creatureLookupSpan) {
            creatureLookupSpan->setSuccess();
        }

        const auto creature = creatureResult.getValue().value();
        if (creature.speech_loop_animation_ids.empty()) {
            auto result = creatures::ws::StatusDto::createShared();
            result->status = "error";
            result->code = 422;
            result->message =
                fmt::format("{} has no speech_loop_animation_ids configured; ad-hoc speech cannot proceed.",
                            creature.name)
                    .c_str();
            if (span) {
                span->setAttribute("error.type", "missing_speech_loop_animation_ids");
                span->setHttpStatus(422);
            }
            return createDtoResponse(Status::CODE_422, result);
        }

        nlohmann::json jobDetails;
        jobDetails["creature_id"] = creatureId;
        jobDetails["text"] = text;
        jobDetails["resume_playlist"] = resumePlaylist;

        auto jobId = creatures::jobManager->createJob(creatures::jobs::JobType::AdHocSpeech, jobDetails.dump());
        creatures::jobWorker->queueJob(jobId);

        auto response = JobCreatedDto::createShared();
        response->job_id = jobId;
        response->job_type = creatures::jobs::toString(creatures::jobs::JobType::AdHocSpeech).c_str();
        response->message = fmt::format(
            "Ad-hoc speech job created for '{}'. Listen for job-progress and job-complete messages.", creatureId);

        if (span) {
            span->setHttpStatus(202);
            span->setAttribute("job.id", jobId);
        }

        return createDtoResponse(Status::CODE_202, response);
    }
};
} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
