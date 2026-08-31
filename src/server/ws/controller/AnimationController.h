#pragma once

#include <algorithm>
#include <fmt/format.h>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "api/AnimationRequests.h"
#include "api/JobResponses.h"
#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/creature/UniverseResolver.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/service/AnimationService.h"
#include "server/ws/service/CreatureService.h"
#include "util/JsonParser.h"
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

inline nlohmann::json animationMetadataListToJson(const std::vector<AnimationMetadata> &items) {
    return api::listResponseToJson(items, animationMetadataToJson);
}

inline nlohmann::json adHocAnimationListToJson(const std::vector<AdHocAnimationSummary> &items) {
    return api::listResponseToJson(items, [](const auto &item) {
        return nlohmann::json{{"animation_id", item.animationId},
                              {"metadata", animationMetadataToJson(item.metadata)},
                              {"created_at", item.createdAt}};
    });
}

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
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation", listAllAnimations, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("REST call to listAllAnimations");
        return runEndpoint("GET /api/v1/animation", "GET", "api/v1/animation", "listAllAnimations",
                           "AnimationController", request, [&](const auto &span) {
                               auto result = m_animationService.listAllAnimations(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto items = result.getValue().value();
                               if (span) {
                                   span->setAttribute("animations.count", static_cast<int64_t>(items.size()));
                                   span->setAttribute("response.items.count", static_cast<int64_t>(items.size()));
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, animationMetadataListToJson(items));
                           });
    }

    ENDPOINT_INFO(listAdHocAnimations) {
        info->summary = "List ad-hoc animations stored in the TTL collection";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc", listAdHocAnimations,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc", "GET", "api/v1/animation/ad-hoc", "listAdHocAnimations",
                           "AnimationController", request, [&](const auto &span) {
                               auto result = m_animationService.listAdHocAnimations(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto items = result.getValue().value();
                               if (span) {
                                   span->setAttribute("response.items.count", static_cast<int64_t>(items.size()));
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, adHocAnimationListToJson(items));
                           });
    }

    ENDPOINT_INFO(getAdHocAnimation) {
        info->summary = "Get an ad-hoc animation by id";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Ad-hoc animation ID";
    }
    ENDPOINT("GET", "api/v1/animation/ad-hoc/{animationId}", getAdHocAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/ad-hoc/{animationId}", "GET", "api/v1/animation/ad-hoc/{animationId}",
                           "getAdHocAnimation", "AnimationController", request, [&](const auto &span) {
                               if (!animationId || !isUuidShape(std::string(animationId)))
                                   return bailHttp(span, Status::CODE_400, "animationId must be a UUID");
                               const auto canonicalAnimationId = canonicalUuid(std::string(animationId));
                               if (span)
                                   span->setAttribute("animation.id", canonicalAnimationId);
                               auto result = m_animationService.getAdHocAnimation(std::string(animationId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto animation = result.getValue().value();
                               if (span) {
                                   span->setAttribute("animation.title", animation.metadata.title);
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, animationToJson(animation));
                           });
    }

    ENDPOINT_INFO(getAnimation) {
        info->summary = "Get an animation by id";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["animationId"].description = "Animation ID in the form of a UUID";
    }
    ENDPOINT("GET", "api/v1/animation/{animationId}", getAnimation, PATH(String, animationId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/animation/{animationId}", "GET", "api/v1/animation/{animationId}",
                           "getAnimation", "AnimationController", request, [&](const auto &span) {
                               if (!animationId || !isUuidShape(std::string(animationId)))
                                   return bailHttp(span, Status::CODE_400, "animationId must be a UUID");
                               const auto canonicalAnimationId = canonicalUuid(std::string(animationId));
                               debug("get animation by ID via REST API: {}", canonicalAnimationId);
                               if (span)
                                   span->setAttribute("animation.id", canonicalAnimationId);
                               auto result = m_animationService.getAnimation(std::string(animationId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto animation = result.getValue().value();
                               if (span) {
                                   span->setAttribute("animation.title", animation.metadata.title);
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, animationToJson(animation));
                           });
    }

    ENDPOINT_INFO(generateLipSyncForAnimation) {
        info->summary = "Generate lip sync for an animation";
        info->description =
            "Queues a background job to derive per-creature lip sync from the animation's multitrack audio.";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/generate-lipsync", generateLipSyncForAnimation,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/generate-lipsync", "POST", "api/v1/animation/generate-lipsync",
            "generateLipSyncForAnimation", "AnimationController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!creatures::db || !creatures::jobManager || !creatures::jobWorker) {
                    if (span)
                        span->setAttribute("error.type", "missing_dependencies");
                    return bailHttp(span, Status::CODE_500,
                                    "Animation lip sync unavailable: server dependencies missing");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "AnimationController.parseRegenerateLipSyncRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "animation.lip_sync.regenerate");
                const auto jsonResult = JsonParser::parseApiJsonString(body, "animation lip sync request", parseSpan);
                if (!jsonResult.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, jsonResult.getError().value());
                }
                const auto requestResult =
                    api::regenerateAnimationLipSyncRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidAnimationLipSyncRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }

                const auto animationId = requestResult.getValue()->animationId;
                if (span) {
                    span->setAttribute("animation.id", canonicalUuid(animationId));
                }

                auto animationResult = m_animationService.getAnimation(animationId, span);
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

                const api::JobCreatedResponse response{
                    jobId, "animation-lip-sync",
                    fmt::format(
                        "Lip sync generation job created for animation {}. Monitor job-progress events for updates.",
                        animationId)};

                if (span) {
                    span->setHttpStatus(202);
                    span->setAttribute("job.id", jobId);
                    span->setAttribute("job.type", "animation-lip-sync");
                }

                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }

    ENDPOINT_INFO(upsertAnimation) {
        info->summary = "Create or update an animation in the database";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_413, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation", upsertAnimation, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("new animation uploaded via REST API");
        return runEndpoint("POST /api/v1/animation", "POST", "api/v1/animation", "upsertAnimation",
                           "AnimationController", request, [&](const auto &span) {
                               auto requestAsString =
                                   readRequestBodyLimited(request, MAX_ANIMATION_REQUEST_BODY_BYTES, span);
                               auto result = m_animationService.upsertAnimation(requestAsString, span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto animation = result.getValue().value();
                               if (span) {
                                   span->setAttribute("animation.id", canonicalUuid(animation.id));
                                   span->setAttribute("animation.title", animation.metadata.title);
                                   span->setHttpStatus(200);
                               }
                               // AnimationService.upsertAnimation goes through storage::publishAnimation,
                               // which fires Animation + SoundList invalidations on success (issue #11 PR #21).
                               return jsonResponse(span, Status::CODE_200, animationToJson(animation));
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
        return runEndpoint("DELETE /api/v1/animation/{animationId}", "DELETE", "api/v1/animation/{animationId}",
                           "deleteAnimation", "AnimationController", request, [&](const auto &span) {
                               if (!animationId || !isUuidShape(std::string(animationId)))
                                   return bailHttp(span, Status::CODE_400, "animationId must be a UUID");
                               const auto canonicalAnimationId = canonicalUuid(std::string(animationId));
                               debug("delete animation via REST API: {}", canonicalAnimationId);
                               if (span)
                                   span->setAttribute("animation.id", canonicalAnimationId);
                               auto result = m_animationService.deleteAnimation(std::string(animationId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               // AnimationService.deleteAnimation goes through storage::deleteAnimation,
                               // which fires the Animation invalidation on success (issue #11 PR #21).
                               // The broadcast-now (vs scheduled-on-event-loop) call below is kept as a
                               // belt-and-suspenders for delete since the deletion is irreversible.
                               auto broadcastResult = broadcastCacheInvalidationToAllClients(CacheType::Animation);
                               if (!broadcastResult.isSuccess()) {
                                   warn("Failed to broadcast animation cache invalidation: {}",
                                        broadcastResult.getError()->getMessage());
                               }
                               return okStatus(span, Status::CODE_200,
                                               fmt::format("Deleted animation {}", canonicalAnimationId));
                           });
    }

    ENDPOINT_INFO(playStoredAnimation) {
        info->summary = "Play one animation out of the database on a given universe";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/play", playStoredAnimation, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/play", "POST", "api/v1/animation/play", "playStoredAnimation",
            "AnimationController", request, [&](const auto &span) {
                if (!creatures::config || !creatures::db) {
                    if (span) {
                        span->setAttribute("error.type", "missing_dependencies");
                    }
                    return bailHttp(span, Status::CODE_500, "Animation play unavailable: server dependencies missing",
                                    nullptr, "MissingDependencies");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "AnimationController.parsePlayRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "animation.play");
                const auto jsonResult = JsonParser::parseApiJsonString(body, "animation play request", parseSpan);
                if (!jsonResult.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, jsonResult.getError().value());
                }
                const auto requestResult = api::playAnimationRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidAnimationPlayRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto parsed = requestResult.getValue().value();

                if (span) {
                    span->setAttribute("animation.id", canonicalUuid(parsed.animationId));
                    span->setAttribute("dmx.universe", static_cast<int64_t>(parsed.universe));
                    span->setAttribute("playback.reason", "play");
                }

                auto result = m_animationService.playStoredAnimation(parsed.animationId, parsed.universe, "play", span);

                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                const auto playback = result.getValue().value();

                if (span) {
                    span->setAttribute("result.message", playback.message);
                    if (playback.sessionId)
                        span->setAttribute("session.id", *playback.sessionId);
                    span->setHttpStatus(200);
                }

                const auto response = api::makeStatusResponse(200, playback.message, STATUS_OK, playback.sessionId);
                return jsonResponse(span, Status::CODE_200, api::statusResponseToJson(response));
            });
    }

    ENDPOINT_INFO(interruptAnimation) {
        info->summary = "Interrupt current playback with a new animation (for interactive Zoom meetings!)";
        info->description = "Cancels the current session on the universe and plays the given animation instead.";
        info->addTag("Animations");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/interrupt", interruptAnimation,
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
                const auto body = readRequestBodyLimited(request, api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "AnimationController.parseInterruptRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "animation.interrupt");
                const auto jsonResult = JsonParser::parseApiJsonString(body, "animation interrupt request", parseSpan);
                if (!jsonResult.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, jsonResult.getError().value());
                }
                const auto requestResult =
                    api::playAnimationRequestFromJson(jsonResult.getValue().value(), "animation interrupt request");
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidAnimationInterruptRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto parsed = requestResult.getValue().value();

                {
                    if (span) {
                        span->setAttribute("animation.id", canonicalUuid(parsed.animationId));
                        span->setAttribute("dmx.universe", static_cast<int64_t>(parsed.universe));
                        span->setAttribute("playback.resume_playlist", parsed.resumePlaylist);
                    }

                    const bool shouldResume = parsed.resumePlaylist;
                    info("REST API: interrupting universe {} with animation {} (resume: {})",
                         static_cast<uint32_t>(parsed.universe), parsed.animationId, shouldResume);

                    // Get the animation from the database
                    auto animationResult = m_animationService.getAnimation(parsed.animationId, span);
                    if (!animationResult.isSuccess())
                        return bailFromServerError(span, animationResult.getError().value());
                    auto animation = animationResult.getValue().value();

                    // Use SessionManager to interrupt
                    auto sessionResult =
                        creatures::sessionManager->interrupt(parsed.universe, animation, shouldResume, span);

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

                    const auto result =
                        api::makeStatusResponse(200, "Animation interrupt scheduled successfully", STATUS_OK,
                                                sessionResult.getValue().value()->getSessionId());
                    return jsonResponse(span, Status::CODE_200, api::statusResponseToJson(result));
                }
            });
    }

    ENDPOINT_INFO(createAdHocAnimation) {
        info->summary = "Generate and play an ad-hoc speech animation";
        info->description =
            "Creates a job that synthesizes audio, generates lip sync, stores a temporary animation, and interrupts.";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc", createAdHocAnimation,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return handleAdHocAnimationRequest(request, creatures::jobs::JobType::AdHocSpeech, true,
                                           "POST /api/v1/animation/ad-hoc", "api/v1/animation/ad-hoc",
                                           "createAdHocAnimation");
    }

    ENDPOINT_INFO(prepareAdHocAnimation) {
        info->summary = "Prepare an ad-hoc speech animation without playing it";
        info->description =
            "Creates the same ad-hoc speech job pipeline but skips the final playback. Use the play endpoint later.";
        info->addTag("Animations");
        info->addResponse<oatpp::String>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_422, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/ad-hoc/prepare", prepareAdHocAnimation,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return handleAdHocAnimationRequest(request, creatures::jobs::JobType::AdHocSpeechPrepare, false,
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

                const auto body = readRequestBodyLimited(request, api::MAX_ANIMATION_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "AnimationController.parseAdHocTriggerRequest", span)
                                           : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "animation.ad_hoc.trigger");
                const auto jsonResult =
                    JsonParser::parseApiJsonString(body, "ad-hoc animation trigger request", parseSpan);
                if (!jsonResult.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, jsonResult.getError().value());
                }
                const auto requestResult = api::triggerAdHocAnimationRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidAdHocAnimationTriggerRequest",
                                    error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto parsed = requestResult.getValue().value();
                const auto &animationId = parsed.animationId;
                const bool resumePlaylist = parsed.resumePlaylist;

                if (span) {
                    span->setAttribute("animation.id", canonicalUuid(animationId));
                    span->setAttribute("playback.resume_playlist", resumePlaylist);
                }

                auto animationResult = m_animationService.getAdHocAnimation(animationId, span);
                if (!animationResult.isSuccess()) {
                    auto error = animationResult.getError().value();
                    if (span) {
                        span->setAttribute("error.type", "adhoc_animation_lookup_failed");
                    }
                    return bailFromServerError(span, error);
                }

                auto animation = animationResult.getValue().value();
                if (animation.tracks.empty()) {
                    if (span) {
                        span->setAttribute("error.type", "empty_animation");
                    }
                    return bailHttp(span, Status::CODE_422, "Prepared animation has no tracks");
                }

                // Every unique creature the animation targets, in track order. Multi-creature
                // animations (rendered dialogs) are fine as long as all of their creatures
                // resolve to one universe — the same rule the dialog render's autoplay uses.
                std::vector<creatureId_t> targetCreatures;
                for (const auto &candidate : animation.tracks) {
                    if (candidate.creature_id.empty()) {
                        if (span) {
                            span->setAttribute("error.type", "missing_creature_id");
                        }
                        return bailHttp(span, Status::CODE_500, "Prepared animation track is missing creature_id");
                    }
                    if (std::find(targetCreatures.begin(), targetCreatures.end(), candidate.creature_id) ==
                        targetCreatures.end()) {
                        targetCreatures.push_back(candidate.creature_id);
                    }
                }

                auto universeResult = creatures::resolveCommonUniverse(targetCreatures);
                if (!universeResult.isSuccess()) {
                    const auto error = universeResult.getError().value();
                    switch (error.getCode()) {
                    case ServerError::Conflict:
                        if (span) {
                            span->setAttribute("error.type", "creature_not_registered");
                        }
                        return bailHttp(span, Status::CODE_409, error.getMessage());
                    case ServerError::InvalidData:
                        if (span) {
                            span->setAttribute("error.type", "multi_universe_animation");
                        }
                        return bailHttp(span, Status::CODE_422, error.getMessage());
                    default:
                        if (span) {
                            span->setAttribute("error.type", "universe_resolution_failed");
                        }
                        return bailHttp(span, Status::CODE_500, error.getMessage());
                    }
                }
                const universe_t universe = universeResult.getValue().value();

                auto sessionResult =
                    creatures::sessionManager->interruptIdleOnly(universe, animation, targetCreatures, span);
                if (!sessionResult.isSuccess()) {
                    if (span) {
                        span->setAttribute("error.type", "session_interrupt_failed");
                    }
                    return bailFromServerError(span, sessionResult.getError().value());
                }

                const auto response = api::makeStatusResponse(
                    200,
                    fmt::format("Triggered ad-hoc animation {} for {} creature(s) on universe {}", animationId,
                                targetCreatures.size(), universe),
                    STATUS_OK, sessionResult.getValue().value()->getSessionId());

                if (span) {
                    span->setAttribute("creature.ids", creatures::joinStrings(targetCreatures, ","));
                    span->setAttribute("dmx.universe", static_cast<int64_t>(universe));
                    span->setAttribute("session.id", sessionResult.getValue().value()->getSessionId());
                    span->setHttpStatus(200);
                }

                return jsonResponse(span, Status::CODE_200, api::statusResponseToJson(response));
            });
    }

  private:
    std::shared_ptr<OutgoingResponse>
    handleAdHocAnimationRequest(const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request,
                                creatures::jobs::JobType jobType, bool autoPlay, const std::string &spanName,
                                const std::string &endpointPath, const std::string &endpointName) {
        return runEndpoint(
            spanName, "POST", endpointPath, endpointName, "AnimationController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (span) {
                    span->setAttribute("playback.auto_play", autoPlay);
                }

                if (!creatures::config || !creatures::db || !creatures::sessionManager || !creatures::jobManager ||
                    !creatures::jobWorker) {
                    if (span) {
                        span->setAttribute("error.type", "missing_dependencies");
                    }
                    return bailHttp(span, Status::CODE_500, "Ad-hoc request unavailable: server dependencies missing");
                }

                const auto body = readRequestBodyLimited(request, api::MAX_AD_HOC_SPEECH_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "AnimationController.parseAdHocRequest", span)
                                                                : nullptr;
                if (parseSpan)
                    parseSpan->setAttribute("validation.contract", "animation.ad_hoc.create");
                const auto jsonResult = JsonParser::parseApiJsonString(body, "ad-hoc animation request", parseSpan);
                if (!jsonResult.isSuccess()) {
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    return bailFromServerError(span, jsonResult.getError().value());
                }
                const auto requestResult = api::createAdHocAnimationRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    if (parseSpan)
                        parseSpan->setAttribute("validation.result", "rejected");
                    recordSpanError(parseSpan, error.getMessage(), "InvalidAdHocAnimationRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan) {
                    parseSpan->setAttribute("validation.result", "accepted");
                    parseSpan->setSuccess();
                }
                const auto parsed = requestResult.getValue().value();
                const auto &creatureId = parsed.creatureId;
                const auto &text = parsed.text;
                const bool resumePlaylist = parsed.resumePlaylist;

                if (span) {
                    span->setAttribute("creature.id", canonicalUuid(creatureId));
                    span->setAttribute("text.length", static_cast<int64_t>(text.size()));
                    span->setAttribute("playback.resume_playlist", resumePlaylist);
                }

                auto creatureResult = CreatureService::getCreature(creatureId, span);
                if (!creatureResult.isSuccess()) {
                    auto error = creatureResult.getError().value();
                    if (span) {
                        span->setAttribute("error.type", "creature_lookup_failed");
                    }
                    return bailFromServerError(span, error);
                }

                const auto creature = creatureResult.getValue()->creature;
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

                api::JobCreatedResponse response{jobId, creatures::jobs::toString(jobType), {}};
                if (autoPlay) {
                    response.message = fmt::format(
                        "Ad-hoc speech job created for '{}'. Listen for job-progress and job-complete messages.",
                        creatureId);
                } else {
                    response.message = fmt::format(
                        "Prepared ad-hoc speech job created for '{}'. Call /api/v1/animation/ad-hoc/play when ready.",
                        creatureId);
                }

                if (span) {
                    span->setHttpStatus(202);
                    span->setAttribute("job.id", jobId);
                    span->setAttribute("job.type", response.jobType);
                }

                return jsonResponse(span, Status::CODE_202, api::jobCreatedResponseToJson(response));
            });
    }
};
} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
