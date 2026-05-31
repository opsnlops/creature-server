#pragma once

#include <algorithm>
#include <fmt/format.h>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/AdHocAnimationDto.h"
#include "server/ws/dto/CreateAdHocAnimationRequestDto.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/PlayAnimationRequestDto.h"
#include "server/ws/dto/RegenerateLipSyncRequestDto.h"
#include "server/ws/dto/TriggerAdHocAnimationRequestDto.h"
#include "server/ws/service/AnimationService.h"
#include "server/ws/service/CreatureService.h"
#include "util/cache.h"
#include "util/websocketUtils.h"
#include <nlohmann/json.hpp>

namespace creatures {
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class AnimationController : public oatpp::web::server::api::ApiController,
                            public HttpResponseHelpers<AnimationController> {
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

    ENDPOINT_INFO(listAllAnimations) {
        info->summary = "List all of the animations";
        info->addTag("Animations");
        info->addResponse<Object<AnimationsListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation", listAllAnimations, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("REST call to listAllAnimations");
        return runEndpoint("GET /api/v1/animation", "GET", "api/v1/animation", "listAllAnimations",
                           "AnimationController", request, [&](const auto &span) {
                               auto result = m_animationService.listAllAnimations(span);
                               if (span) {
                                   span->setAttribute("animations.count", static_cast<int64_t>(result->count));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(listAdHocAnimations) {
        info->summary = "List ad-hoc animations stored in the TTL collection";
        info->addTag("Animations");
        info->addResponse<Object<AdHocAnimationListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc", listAdHocAnimations,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc", "GET", "api/v1/animation/ad-hoc", "listAdHocAnimations",
                           "AnimationController", request, [&](const auto &span) {
                               auto result = m_animationService.listAdHocAnimations(span);
                               if (span) {
                                   span->setAttribute("adhoc.count", static_cast<int64_t>(result->count));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(getAdHocAnimation) {
        info->summary = "Get an ad-hoc animation by id";
        info->addTag("Animations");
        info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Ad-hoc animation ID";
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc/{animationId}", getAdHocAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc/{animationId}", "GET",
                           "api/v1/animation/ad-hoc/" + std::string(animationId), "getAdHocAnimation",
                           "AnimationController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("animation.id", std::string(animationId));
                               auto result = m_animationService.getAdHocAnimation(animationId, span);
                               if (span) {
                                   span->setAttribute("animation.title", std::string(result->metadata->title));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(getAnimation) {
        info->summary = "Get an animation by id";
        info->addTag("Animations");
        info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Animation ID in the form of a MongoDB OID";
    }
    ENDPOINT("GET", "api/v1/animation/{animationId}", getAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("get animation by ID via REST API: {}", std::string(animationId));
        return runEndpoint("GET /api/v1/animation/{animationId}", "GET", "api/v1/animation/" + std::string(animationId),
                           "getAnimation", "AnimationController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("animation.id", std::string(animationId));
                               auto result = m_animationService.getAnimation(animationId, span);
                               if (span) {
                                   span->setAttribute("animation.title", std::string(result->metadata->title));
                                   span->setHttpStatus(200);
                               }
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(generateLipSyncForAnimation) {
        info->summary = "Generate lip sync for an animation";
        info->description =
            "Queues a background job to derive per-creature lip sync from the animation's multitrack audio.";
        info->addTag("Animations");
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/generate-lipsync", generateLipSyncForAnimation,
             BODY_DTO(Object<creatures::ws::RegenerateLipSyncRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/generate-lipsync", "POST", "api/v1/animation/generate-lipsync",
            "generateLipSyncForAnimation", "AnimationController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!requestBody || !requestBody->animation_id || requestBody->animation_id->empty()) {
                    if (span) {
                        span->setAttribute("error.type", "invalid_request");
                    }
                    return bailHttp(span, Status::CODE_400, "animation_id is required");
                }

                std::string animationId = std::string(requestBody->animation_id);
                if (span) {
                    span->setAttribute("animation.id", animationId);
                }

                auto animationLookupSpan =
                    creatures::observability->createOperationSpan("AnimationController.getAnimation", span);
                if (animationLookupSpan) {
                    animationLookupSpan->setAttribute("animation.id", animationId);
                }

                auto animationResult = creatures::db->getAnimation(animationId, animationLookupSpan);
                if (!animationResult.isSuccess()) {
                    if (span) {
                        span->setAttribute("error.type", "animation_lookup_failed");
                    }
                    return bailFromServerError(span, animationResult.getError().value());
                }
                auto animation = animationResult.getValue().value();

                if (animation.metadata.sound_file.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "missing_sound_file");
                    }
                    return bailHttp(span, Status::CODE_422, "Animation has no sound file configured");
                }

                if (!animation.metadata.multitrack_audio) {
                    if (span) {
                        span->setAttribute("error.type", "audio_not_multitrack");
                    }
                    return bailHttp(span, Status::CODE_422, "Animation audio must be multitrack to generate lip sync");
                }

                nlohmann::json jobDetails;
                jobDetails["animation_id"] = animationId;

                auto jobId = creatures::jobManager->createJob(creatures::jobs::JobType::AnimationLipSync,
                                                              jobDetails.dump(), span);
                creatures::jobWorker->queueJob(jobId);

                auto response = JobCreatedDto::createShared();
                response->job_id = jobId;
                response->job_type = "animation-lip-sync";
                response->message = fmt::format(
                    "Lip sync generation job created for animation {}. Monitor job-progress events for updates.",
                    animationId);

                if (span) {
                    span->setHttpStatus(202);
                    span->setAttribute("job.id", jobId);
                }

                return createDtoResponse(Status::CODE_202, response);
            });
    }

    ENDPOINT_INFO(upsertAnimation) {
        info->summary = "Create or update an animation in the database";
        info->addTag("Animations");
        info->addResponse<Object<AnimationDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation", upsertAnimation, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("new animation uploaded via REST API");
        return runEndpoint("POST /api/v1/animation", "POST", "api/v1/animation", "upsertAnimation",
                           "AnimationController", request, [&](const auto &span) {
                               auto requestAsString = std::string(request->readBodyToString());
                               trace("request was: {}", requestAsString);
                               if (span) {
                                   span->setAttribute("request.body_size",
                                                      static_cast<int64_t>(requestAsString.length()));
                               }
                               auto result = m_animationService.upsertAnimation(requestAsString, span);
                               if (span) {
                                   span->setAttribute("animation.id", std::string(result->id));
                                   span->setAttribute("animation.title", std::string(result->metadata->title));
                                   span->setHttpStatus(200);
                               }
                               scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(deleteAnimation) {
        info->summary = "Delete an animation and all of its tracks";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Animation ID";
    }
    ENDPOINT("DELETE", "api/v1/animation/{animationId}", deleteAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("delete animation via REST API: {}", std::string(animationId));
        return runEndpoint("DELETE /api/v1/animation/{animationId}", "DELETE",
                           fmt::format("api/v1/animation/{}", std::string(animationId)), "deleteAnimation",
                           "AnimationController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("animation.id", std::string(animationId));
                               auto result = m_animationService.deleteAnimation(animationId, span);
                               if (span)
                                   span->setHttpStatus(200);
                               scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Animation);
                               auto broadcastResult = broadcastCacheInvalidationToAllClients(CacheType::Animation);
                               if (!broadcastResult.isSuccess()) {
                                   warn("Failed to broadcast animation cache invalidation: {}",
                                        broadcastResult.getError()->getMessage());
                               }
                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(playStoredAnimation) {
        info->summary = "Play one animation out of the database on a given universe";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/play", playStoredAnimation,
             BODY_DTO(Object<creatures::ws::PlayAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/animation/play", "POST", "api/v1/animation/play", "playStoredAnimation",
                           "AnimationController", request, [&](const auto &span) {
                               if (!creatures::config || !creatures::db) {
                                   if (span) {
                                       span->setAttribute("error.type", "missing_dependencies");
                                   }
                                   return bailHttp(span, Status::CODE_500,
                                                   "Animation play unavailable: server dependencies missing");
                               }

                               if (span) {
                                   span->setAttribute("animation.id", std::string(requestBody->animation_id));
                                   span->setAttribute("universe", static_cast<int64_t>(requestBody->universe));
                                   span->setAttribute("reason", "play");
                               }

                               auto result = m_animationService.playStoredAnimation(
                                   std::string(requestBody->animation_id), requestBody->universe, "play");

                               if (span) {
                                   span->setAttribute("result.message", std::string(result->message));
                                   span->setHttpStatus(200);
                               }

                               return createDtoResponse(Status::CODE_200, result);
                           });
    }

    ENDPOINT_INFO(interruptAnimation) {
        info->summary = "Interrupt current playback with a new animation (for interactive Zoom meetings!)";
        info->description =
            "Requires cooperative scheduler (--scheduler cooperative). Returns 400 if legacy scheduler is active.";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/interrupt", interruptAnimation,
             BODY_DTO(Object<creatures::ws::PlayAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/interrupt", "POST", "api/v1/animation/interrupt", "interruptAnimation",
            "AnimationController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!creatures::config || !creatures::sessionManager) {
                    if (span) {
                        span->setAttribute("error.type", "missing_dependencies");
                    }
                    return bailHttp(span, Status::CODE_500,
                                    "Animation interrupt unavailable: server dependencies missing");
                }

                // Check if cooperative scheduler is enabled
                if (creatures::config->getAnimationSchedulerType() !=
                    creatures::Configuration::AnimationSchedulerType::Cooperative) {
                    const char *msg = "Animation interrupts require the cooperative scheduler. Start server with "
                                      "--scheduler cooperative";
                    if (span) {
                        span->setAttribute("error.type", "scheduler_not_supported");
                        span->setAttribute("error.message", std::string(msg));
                        span->setAttribute("scheduler_type", "legacy");
                    }
                    error("Interrupt API called with legacy scheduler enabled");
                    return bailHttp(span, Status::CODE_400, msg);
                }

                {
                    if (span) {
                        span->setAttribute("animation.id", std::string(requestBody->animation_id));
                        span->setAttribute("universe", static_cast<int64_t>(requestBody->universe));
                        span->setAttribute("resume_playlist", static_cast<bool>(requestBody->resumePlaylist));
                    }

                    bool shouldResume = requestBody->resumePlaylist ? true : false;
                    info("REST API: interrupting universe {} with animation {} (resume: {})",
                         static_cast<uint32_t>(requestBody->universe), std::string(requestBody->animation_id),
                         shouldResume);

                    // Get the animation from the database
                    auto animationDto = m_animationService.getAnimation(requestBody->animation_id, span);

                    // Convert from oatpp DTO to internal model
                    std::shared_ptr<AnimationDto> animationDtoPtr(animationDto.get(),
                                                                  [](AnimationDto *) {}); // Non-owning shared_ptr
                    auto animation = convertFromDto(animationDtoPtr);

                    // Use SessionManager to interrupt
                    auto sessionResult =
                        creatures::sessionManager->interrupt(requestBody->universe, animation, shouldResume, span);

                    if (!sessionResult.isSuccess()) {
                        auto errorMsg = sessionResult.getError()->getMessage();
                        error("Failed to interrupt animation: {}", errorMsg);
                        if (span) {
                            span->setAttribute("error.message", errorMsg);
                        }
                        return bailFromServerError(span, sessionResult.getError().value());
                    }

                    // Success! Custom DTO so we can stamp session_id.
                    if (span) {
                        span->setAttribute("result.success", true);
                        span->setHttpStatus(200);
                        span->setAttribute("session.id", sessionResult.getValue().value()->getSessionId());
                    }

                    auto result = creatures::ws::StatusDto::createShared();
                    result->status = STATUS_OK;
                    result->code = 200;
                    result->message = "Animation interrupt scheduled successfully";
                    result->session_id = sessionResult.getValue().value()->getSessionId().c_str();

                    return createDtoResponse(Status::CODE_200, result);
                }
            });
    }

    ENDPOINT_INFO(createAdHocAnimation) {
        info->summary = "Generate and play an ad-hoc speech animation";
        info->description =
            "Creates a job that synthesizes audio, generates lip sync, stores a temporary animation, and interrupts.";
        info->addTag("Animations");
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc", createAdHocAnimation,
             BODY_DTO(Object<creatures::ws::CreateAdHocAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return handleAdHocAnimationRequest(requestBody, request, creatures::jobs::JobType::AdHocSpeech, true,
                                           "POST /api/v1/animation/ad-hoc", "api/v1/animation/ad-hoc",
                                           "createAdHocAnimation");
    }

    ENDPOINT_INFO(prepareAdHocAnimation) {
        info->summary = "Prepare an ad-hoc speech animation without playing it";
        info->description =
            "Creates the same ad-hoc speech job pipeline but skips the final playback. Use the play endpoint later.";
        info->addTag("Animations");
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc/prepare", prepareAdHocAnimation,
             BODY_DTO(Object<creatures::ws::CreateAdHocAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return handleAdHocAnimationRequest(requestBody, request, creatures::jobs::JobType::AdHocSpeechPrepare, false,
                                           "POST /api/v1/animation/ad-hoc/prepare", "api/v1/animation/ad-hoc/prepare",
                                           "prepareAdHocAnimation");
    }

    ENDPOINT_INFO(playPreparedAdHocAnimation) {
        info->summary = "Play a prepared ad-hoc animation";
        info->description =
            "Loads an ad-hoc animation from the TTL cache and interrupts the current universe without regenerating.";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_409, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc/play", playPreparedAdHocAnimation,
             BODY_DTO(Object<creatures::ws::TriggerAdHocAnimationRequestDto>, requestBody),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/ad-hoc/play", "POST", "api/v1/animation/ad-hoc/play", "playPreparedAdHocAnimation",
            "AnimationController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!creatures::config || !creatures::db || !creatures::sessionManager ||
                    !creatures::creatureUniverseMap) {
                    if (span) {
                        span->setAttribute("error.type", "missing_dependencies");
                    }
                    return bailHttp(span, Status::CODE_500, "Ad-hoc play unavailable: server dependencies missing");
                }

                if (creatures::config->getAnimationSchedulerType() !=
                    creatures::Configuration::AnimationSchedulerType::Cooperative) {
                    if (span) {
                        span->setAttribute("error.type", "scheduler_not_supported");
                    }
                    return bailHttp(span, Status::CODE_400,
                                    "Ad-hoc speech requires the cooperative scheduler (--scheduler cooperative)");
                }

                auto animationId = requestBody->animation_id ? std::string(requestBody->animation_id) : "";
                bool resumePlaylist =
                    requestBody->resume_playlist ? static_cast<bool>(requestBody->resume_playlist) : true;

                if (span) {
                    span->setAttribute("animation.id", animationId);
                    span->setAttribute("resume_playlist", resumePlaylist);
                }

                if (animationId.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "invalid_request");
                    }
                    return bailHttp(span, Status::CODE_400, "animation_id is required");
                }

                auto animationLookupSpan =
                    creatures::observability
                        ? creatures::observability->createOperationSpan("AnimationController.getAdHocAnimation", span)
                        : nullptr;
                if (animationLookupSpan) {
                    animationLookupSpan->setAttribute("animation.id", animationId);
                }

                auto animationResult = creatures::db->getAdHocAnimation(animationId, animationLookupSpan);
                if (!animationResult.isSuccess()) {
                    auto error = animationResult.getError().value();
                    if (span) {
                        span->setAttribute("error.type", "adhoc_animation_lookup_failed");
                    }
                    if (animationLookupSpan) {
                        animationLookupSpan->setError(error.getMessage());
                    }
                    return bailFromServerError(span, error);
                }

                auto animation = animationResult.getValue().value();
                if (animationLookupSpan) {
                    animationLookupSpan->setSuccess();
                }
                if (animation.tracks.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "empty_animation");
                    }
                    return bailHttp(span, Status::CODE_422, "Prepared animation has no tracks");
                }

                const auto &track = animation.tracks.front();
                auto creatureId = track.creature_id;
                auto mismatchedTrack =
                    std::any_of(animation.tracks.begin(), animation.tracks.end(),
                                [&](const creatures::Track &candidate) { return candidate.creature_id != creatureId; });
                if (mismatchedTrack) {
                    if (span) {
                        span->setAttribute("error.type", "multi_creature_animation");
                    }
                    return bailHttp(span, Status::CODE_422,
                                    "Prepared animation targets multiple creatures; cannot auto-play.");
                }

                if (creatureId.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "missing_creature_id");
                    }
                    return bailHttp(span, Status::CODE_500, "Prepared animation track is missing creature_id");
                }

                universe_t universe;
                try {
                    auto universePtr = creatures::creatureUniverseMap->get(creatureId);
                    universe = *universePtr;
                } catch (const std::exception &) {
                    if (span) {
                        span->setAttribute("error.type", "creature_not_registered");
                    }
                    return bailHttp(
                        span, Status::CODE_409,
                        fmt::format("Creature {} is not registered with a universe. Is the controller online?",
                                    creatureId));
                }

                auto sessionResult =
                    creatures::sessionManager->interruptIdleOnly(universe, animation, std::string(creatureId), span);
                if (!sessionResult.isSuccess()) {
                    if (span) {
                        span->setAttribute("error.type", "session_interrupt_failed");
                    }
                    return bailFromServerError(span, sessionResult.getError().value());
                }

                // Success — custom DTO carries session_id alongside the standard envelope.
                auto response = creatures::ws::StatusDto::createShared();
                response->status = STATUS_OK;
                response->code = 200;
                response->message = fmt::format("Triggered ad-hoc animation {} for {} on universe {}", animationId,
                                                creatureId, universe)
                                        .c_str();
                response->session_id = sessionResult.getValue().value()->getSessionId().c_str();

                if (span) {
                    span->setAttribute("creature.id", creatureId);
                    span->setAttribute("universe", static_cast<int64_t>(universe));
                    span->setAttribute("session.id", sessionResult.getValue().value()->getSessionId());
                    span->setHttpStatus(200);
                }

                return createDtoResponse(Status::CODE_200, response);
            });
    }

  private:
    std::shared_ptr<OutgoingResponse>
    handleAdHocAnimationRequest(const oatpp::Object<creatures::ws::CreateAdHocAnimationRequestDto> &requestBody,
                                const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request,
                                creatures::jobs::JobType jobType, bool autoPlay, const std::string &spanName,
                                const std::string &endpointPath, const std::string &endpointName) {
        return runEndpoint(
            spanName, "POST", endpointPath, endpointName, "AnimationController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (span) {
                    span->setAttribute("auto_play", autoPlay);
                }

                if (!creatures::config || !creatures::db || !creatures::sessionManager) {
                    if (span) {
                        span->setAttribute("error.type", "missing_dependencies");
                    }
                    return bailHttp(span, Status::CODE_500, "Ad-hoc request unavailable: server dependencies missing");
                }

                if (creatures::config->getAnimationSchedulerType() !=
                    creatures::Configuration::AnimationSchedulerType::Cooperative) {
                    if (span) {
                        span->setAttribute("error.type", "scheduler_not_supported");
                    }
                    return bailHttp(span, Status::CODE_400,
                                    "Ad-hoc speech requires the cooperative scheduler (--scheduler cooperative)");
                }

                auto creatureId = requestBody->creature_id ? std::string(requestBody->creature_id) : "";
                auto text = requestBody->text ? std::string(requestBody->text) : "";
                bool resumePlaylist =
                    requestBody->resume_playlist ? static_cast<bool>(requestBody->resume_playlist) : true;

                if (span) {
                    span->setAttribute("creature.id", creatureId);
                    span->setAttribute("text.length", static_cast<int64_t>(text.size()));
                    span->setAttribute("resume_playlist", resumePlaylist);
                }

                if (creatureId.empty() || text.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "invalid_request");
                    }
                    return bailHttp(span, Status::CODE_400, "creature_id and text are required");
                }

                auto creatureLookupSpan =
                    creatures::observability->createOperationSpan("AnimationController.lookupCreature", span);
                if (creatureLookupSpan) {
                    creatureLookupSpan->setAttribute("creature.id", creatureId);
                }

                auto creatureResult = creatures::db->getCreature(creatureId, creatureLookupSpan);
                if (!creatureResult.isSuccess()) {
                    auto error = creatureResult.getError().value();
                    const int statusCode = creatures::serverErrorToStatusCode(error.getCode());
                    if (creatureLookupSpan) {
                        creatureLookupSpan->setError(error.getMessage());
                        creatureLookupSpan->setAttribute("error.code", static_cast<int64_t>(statusCode));
                    }
                    if (span) {
                        span->setAttribute("error.type", "creature_lookup_failed");
                    }
                    return bailFromServerError(span, error);
                }

                if (creatureLookupSpan) {
                    creatureLookupSpan->setSuccess();
                }

                const auto creature = creatureResult.getValue().value();
                if (creature.speech_loop_animation_ids.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "missing_speech_loop_animation_ids");
                    }
                    return bailHttp(
                        span, Status::CODE_422,
                        fmt::format("{} has no speech_loop_animation_ids configured; ad-hoc speech cannot proceed.",
                                    creature.name));
                }

                nlohmann::json jobDetails;
                jobDetails["creature_id"] = creatureId;
                jobDetails["text"] = text;
                jobDetails["resume_playlist"] = resumePlaylist;
                jobDetails["auto_play"] = autoPlay;

                auto jobId = creatures::jobManager->createJob(jobType, jobDetails.dump(), span);
                creatures::jobWorker->queueJob(jobId);

                auto response = JobCreatedDto::createShared();
                response->job_id = jobId;
                response->job_type = creatures::jobs::toString(jobType).c_str();
                if (autoPlay) {
                    response->message = fmt::format(
                        "Ad-hoc speech job created for '{}'. Listen for job-progress and job-complete messages.",
                        creatureId);
                } else {
                    response->message = fmt::format(
                        "Prepared ad-hoc speech job created for '{}'. Call /api/v1/animation/ad-hoc/play when ready.",
                        creatureId);
                }

                if (span) {
                    span->setHttpStatus(202);
                    span->setAttribute("job.id", jobId);
                }

                return createDtoResponse(Status::CODE_202, response);
            });
    }
};
} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
