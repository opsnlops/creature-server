#include "server/transport/AnimationHandlers.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "api/AnimationRequests.h"
#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "model/Animation.h"
#include "server/animation/SessionManager.h"
#include "server/creature/UniverseResolver.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/storage/Storage.h"
#include "server/ws/service/CreatureService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
} // namespace creatures

namespace creatures::transport {
namespace {
PreparedResponse failure(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    const auto code = serverErrorToStatusCode(error.getCode());
    recordSpanError(span, error.getMessage(), "AnimationOperationFailed", error.getCode());
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, error.getMessage()))));
}
PreparedResponse status(const int code, std::string message, const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        code >= 400 ? span->setError(message) : span->setSuccess();
    }
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, std::move(message)))));
}
template <typename Parser>
auto parse(const std::string &body, const std::string &context, const std::shared_ptr<OperationSpan> &span,
           Parser parser) {
    const auto json = JsonParser::parseApiJsonString(body, context, span);
    using ResultType = decltype(parser(nlohmann::json{}));
    if (!json.isSuccess()) {
        return ResultType{json.getError().value()};
    }
    return parser(json.getValue().value());
}
} // namespace

PreparedResponse listAnimations(const std::shared_ptr<OperationSpan> &span) {
    const auto result = creatures::db->listAnimations(SortBy::name, span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    const auto items = result.getValue().value();
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::listResponseToJson(items, animationMetadataToJson)));
}

PreparedResponse listAdHocAnimations(const std::shared_ptr<OperationSpan> &span) {
    const auto result = creatures::db->listAdHocAnimations(span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    const auto records = result.getValue().value();
    nlohmann::json items = nlohmann::json::array();
    for (const auto &record : records) {
        items.push_back({{"animation_id", record.animation.id},
                         {"metadata", animationMetadataToJson(record.animation.metadata)},
                         {"created_at", formatTimeISO8601(record.createdAt)}});
    }
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, nlohmann::json{{"count", items.size()}, {"items", items}}.dump());
}

PreparedResponse getAnimation(const std::string &id, const bool adHoc, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(id))
        return status(400, "animationId must be a UUID", span);
    const auto result = adHoc ? creatures::db->getAdHocAnimation(id, span) : creatures::db->getAnimation(id, span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, animationToJson(result.getValue().value()).dump());
}

PreparedResponse upsertAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto result = storage::publishAnimation(body, span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, animationToJson(result.getValue().value()).dump());
}

PreparedResponse deleteAnimation(const std::string &id, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(id))
        return status(400, "animationId must be a UUID", span);
    const auto result = storage::deleteAnimation(id, span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    return status(200, fmt::format("Deleted animation {}", canonicalUuid(id)), span);
}

PreparedResponse playAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parse(body, "animation play request", span,
                              [](const auto &json) { return api::playAnimationRequestFromJson(json); });
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const auto result = creatures::db->playStoredAnimation(request.animationId, request.universe, span);
    if (!result.isSuccess())
        return failure(result.getError().value(), span);
    std::optional<std::string> sessionId;
    if (creatures::sessionManager) {
        if (const auto session = creatures::sessionManager->getCurrentSession(request.universe)) {
            sessionId = session->getSessionId();
        }
    }
    if (span)
        span->setSuccess();
    return PreparedResponse::json(200, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(
                                           200, result.getValue().value(), api::STATUS_OK, std::move(sessionId)))));
}

PreparedResponse regenerateAnimationLipSync(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parse(body, "animation lip sync request", span,
                              [](const auto &json) { return api::regenerateAnimationLipSyncRequestFromJson(json); });
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto animationId = parsed.getValue()->animationId;
    const auto animationResult = creatures::db->getAnimation(animationId, span);
    if (!animationResult.isSuccess())
        return failure(animationResult.getError().value(), span);
    const auto animation = animationResult.getValue().value();
    if (animation.metadata.sound_file.empty())
        return status(422, "Animation has no sound file configured", span);
    if (!animation.metadata.multitrack_audio)
        return status(422, "Animation audio must be multitrack to generate lip sync", span);
    const auto jobId = creatures::jobManager->createJob(jobs::JobType::AnimationLipSync,
                                                        nlohmann::json{{"animation_id", animationId}}.dump(), span);
    creatures::jobWorker->queueJob(jobId);
    if (span)
        span->setSuccess();
    const api::JobCreatedResponse response{
        jobId, "animation-lip-sync",
        fmt::format("Lip sync generation job created for animation {}. Monitor job-progress events for updates.",
                    animationId)};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

PreparedResponse createAdHocAnimation(const std::string &body, const bool autoPlay,
                                      const std::shared_ptr<OperationSpan> &span) {
    const auto parsed = parse(body, "ad-hoc animation request", span,
                              [](const auto &json) { return api::createAdHocAnimationRequestFromJson(json); });
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const auto creature = ws::CreatureService::getCreature(request.creatureId, span);
    if (!creature.isSuccess())
        return failure(creature.getError().value(), span);
    if (creature.getValue()->creature.speech_loop_animation_ids.empty()) {
        return status(422,
                      fmt::format("{} has no speech_loop_animation_ids configured; ad-hoc speech cannot proceed.",
                                  creature.getValue()->creature.name),
                      span);
    }
    const auto type = autoPlay ? jobs::JobType::AdHocSpeech : jobs::JobType::AdHocSpeechPrepare;
    const auto details = nlohmann::json{{"creature_id", request.creatureId},
                                        {"text", request.text},
                                        {"resume_playlist", request.resumePlaylist},
                                        {"auto_play", autoPlay}};
    const auto jobId = creatures::jobManager->createJob(type, details.dump(), span);
    creatures::jobWorker->queueJob(jobId);
    if (span)
        span->setSuccess();
    const api::JobCreatedResponse response{
        jobId, jobs::toString(type),
        autoPlay
            ? fmt::format("Ad-hoc speech job created for '{}'. Listen for job-progress and job-complete messages.",
                          request.creatureId)
            : fmt::format("Prepared ad-hoc speech job created for '{}'. Call /api/v1/animation/ad-hoc/play when ready.",
                          request.creatureId)};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

PreparedResponse interruptAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::sessionManager || !creatures::db)
        return status(500, "Animation interrupt unavailable: server dependencies missing", span);
    const auto parsed = parse(body, "animation interrupt request", span, [](const auto &json) {
        return api::playAnimationRequestFromJson(json, "animation interrupt request");
    });
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    if (span) {
        span->setAttribute("animation.id", canonicalUuid(request.animationId));
        span->setAttribute("dmx.universe", static_cast<int64_t>(request.universe));
        span->setAttribute("playback.resume_playlist", request.resumePlaylist);
    }
    const auto animation = creatures::db->getAnimation(request.animationId, span);
    if (!animation.isSuccess())
        return failure(animation.getError().value(), span);
    const auto session = creatures::sessionManager->interrupt(request.universe, animation.getValue().value(),
                                                              request.resumePlaylist, span);
    if (!session.isSuccess())
        return failure(session.getError().value(), span);
    const auto sessionId = session.getValue().value()->getSessionId();
    if (span) {
        span->setAttribute("session.id", sessionId);
        span->setSuccess();
    }
    return PreparedResponse::json(200,
                                  api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(
                                      200, "Animation interrupt scheduled successfully", api::STATUS_OK, sessionId))));
}

PreparedResponse playPreparedAdHocAnimation(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::sessionManager || !creatures::db || !creatures::creatureUniverseMap)
        return status(500, "Ad-hoc play unavailable: server dependencies missing", span);
    const auto parsed = parse(body, "ad-hoc animation trigger request", span,
                              [](const auto &json) { return api::triggerAdHocAnimationRequestFromJson(json); });
    if (!parsed.isSuccess())
        return failure(parsed.getError().value(), span);
    const auto request = parsed.getValue().value();
    const auto animationResult = creatures::db->getAdHocAnimation(request.animationId, span);
    if (!animationResult.isSuccess())
        return failure(animationResult.getError().value(), span);
    const auto animation = animationResult.getValue().value();
    if (animation.tracks.empty())
        return status(422, "Prepared animation has no tracks", span);
    std::vector<creatureId_t> targetCreatures;
    for (const auto &track : animation.tracks) {
        if (track.creature_id.empty())
            return status(500, "Prepared animation track is missing creature_id", span);
        if (std::find(targetCreatures.begin(), targetCreatures.end(), track.creature_id) == targetCreatures.end())
            targetCreatures.push_back(track.creature_id);
    }
    const auto universeResult = creatures::resolveCommonUniverse(targetCreatures);
    if (!universeResult.isSuccess()) {
        const auto error = universeResult.getError().value();
        switch (error.getCode()) {
        case ServerError::Conflict:
            if (span)
                span->setAttribute("error.type", "creature_not_registered");
            return status(409, error.getMessage(), span);
        case ServerError::InvalidData:
            if (span)
                span->setAttribute("error.type", "multi_universe_animation");
            return status(422, error.getMessage(), span);
        default:
            if (span)
                span->setAttribute("error.type", "universe_resolution_failed");
            return status(500, error.getMessage(), span);
        }
    }
    const auto universe = universeResult.getValue().value();
    const auto session = creatures::sessionManager->interruptIdleOnly(universe, animation, targetCreatures, span);
    if (!session.isSuccess())
        return failure(session.getError().value(), span);
    const auto sessionId = session.getValue().value()->getSessionId();
    if (span) {
        span->setAttribute("animation.id", canonicalUuid(request.animationId));
        span->setAttribute("creature.ids", creatures::joinStrings(targetCreatures, ","));
        span->setAttribute("dmx.universe", static_cast<int64_t>(universe));
        span->setAttribute("session.id", sessionId);
        span->setSuccess();
    }
    return PreparedResponse::json(
        200, api::jsonToString(api::statusResponseToJson(
                 api::makeStatusResponse(200,
                                         fmt::format("Triggered ad-hoc animation {} for {} creature(s) on universe {}",
                                                     request.animationId, targetCreatures.size(), universe),
                                         api::STATUS_OK, sessionId))));
}
} // namespace creatures::transport
