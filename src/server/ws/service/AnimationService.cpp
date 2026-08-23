#include "AnimationService.h"

#include <string>
#include <utility>
#include <vector>

#include "server/animation/SessionManager.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/storage/Storage.h"
#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<Configuration> config;
} // namespace creatures

namespace creatures::ws {

namespace {

const char *errorType(ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::Forbidden:
        return "Forbidden";
    case ServerError::InvalidData:
        return "InvalidData";
    case ServerError::DatabaseError:
        return "DatabaseError";
    case ServerError::Conflict:
        return "Conflict";
    default:
        return "InternalError";
    }
}

template <typename T> Result<T> fail(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    warn("{}", error.getMessage());
    recordSpanError(span, error.getMessage(), errorType(error.getCode()), error.getCode());
    return Result<T>{error};
}

} // namespace

Result<std::vector<AnimationMetadata>> AnimationService::listAllAnimations(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.listAllAnimations", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "listAllAnimations");
    }

    auto result = db->listAnimations(SortBy::name, span);
    if (!result.isSuccess())
        return fail<std::vector<AnimationMetadata>>(result.getError().value(), span);

    auto metadata = result.getValue().value();
    if (span) {
        span->setAttribute("animations.count", static_cast<int64_t>(metadata.size()));
        span->setSuccess();
    }
    return Result<std::vector<AnimationMetadata>>{metadata};
}

Result<std::vector<AdHocAnimationSummary>>
AnimationService::listAdHocAnimations(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.listAdHocAnimations", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "listAdHocAnimations");
    }

    auto result = db->listAdHocAnimations(span);
    if (!result.isSuccess())
        return fail<std::vector<AdHocAnimationSummary>>(result.getError().value(), span);

    std::vector<AdHocAnimationSummary> summaries;
    const auto records = result.getValue().value();
    summaries.reserve(records.size());
    for (const auto &record : records) {
        summaries.push_back({record.animation.id, record.animation.metadata, formatTimeISO8601(record.createdAt)});
    }
    if (span) {
        span->setAttribute("adhoc.count", static_cast<int64_t>(summaries.size()));
        span->setSuccess();
    }
    return Result<std::vector<AdHocAnimationSummary>>{summaries};
}

Result<Animation> AnimationService::getAnimation(const std::string &animationId,
                                                 std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.getAnimation", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "getAnimation");
        span->setAttribute("animation.id", animationId);
    }

    auto result = db->getAnimation(animationId, span);
    if (!result.isSuccess())
        return fail<Animation>(result.getError().value(), span);

    auto animation = result.getValue().value();
    if (span) {
        span->setAttribute("animation.title", animation.metadata.title);
        span->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        span->setAttribute("animation.number_of_frames", static_cast<int64_t>(animation.metadata.number_of_frames));
        span->setAttribute("animation.milliseconds_per_frame",
                           static_cast<int64_t>(animation.metadata.milliseconds_per_frame));
        span->setSuccess();
    }
    return Result<Animation>{animation};
}

Result<Animation> AnimationService::getAdHocAnimation(const std::string &animationId,
                                                      std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.getAdHocAnimation", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "getAdHocAnimation");
        span->setAttribute("animation.id", animationId);
    }

    auto result = db->getAdHocAnimation(animationId, span);
    if (!result.isSuccess())
        return fail<Animation>(result.getError().value(), span);

    auto animation = result.getValue().value();
    if (span) {
        span->setAttribute("animation.title", animation.metadata.title);
        span->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        span->setAttribute("animation.number_of_frames", static_cast<int64_t>(animation.metadata.number_of_frames));
        span->setAttribute("animation.milliseconds_per_frame",
                           static_cast<int64_t>(animation.metadata.milliseconds_per_frame));
        span->setSuccess();
    }
    return Result<Animation>{animation};
}

Result<Animation> AnimationService::upsertAnimation(const std::string &animationJson,
                                                    std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.upsertAnimation", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "upsertAnimation");
        span->setAttribute("json.size_bytes", static_cast<int64_t>(animationJson.size()));
    }

    auto result = storage::publishAnimation(animationJson, span);
    if (!result.isSuccess())
        return fail<Animation>(result.getError().value(), span);
    if (!result.getValue().has_value())
        return fail<Animation>(
            ServerError(ServerError::InternalError, "Animation upsert succeeded without returning an animation"), span);

    auto animation = result.getValue().value();
    info("Updated animation '{}' in the database (id: {})", animation.metadata.title, animation.id);
    if (span) {
        span->setAttribute("animation.id", animation.id);
        span->setAttribute("animation.title", animation.metadata.title);
        span->setSuccess();
    }
    return Result<Animation>{animation};
}

Result<void> AnimationService::deleteAnimation(const std::string &animationId,
                                               std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.deleteAnimation", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "deleteAnimation");
        span->setAttribute("animation.id", animationId);
    }

    auto result = storage::deleteAnimation(animationId, span);
    if (!result.isSuccess())
        return fail<void>(result.getError().value(), span);
    if (span)
        span->setSuccess();
    return Result<void>{};
}

Result<PlayAnimationResult> AnimationService::playStoredAnimation(const std::string &animationId, universe_t universe,
                                                                  const std::string &reason,
                                                                  std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability->createOperationSpan("AnimationService.playStoredAnimation", std::move(parentSpan));
    if (span) {
        span->setAttribute("service", "AnimationService");
        span->setAttribute("operation", "playStoredAnimation");
        span->setAttribute("animation.id", animationId);
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("reason", reason);
    }
    if (!db)
        return fail<PlayAnimationResult>(
            ServerError(ServerError::InternalError, "Animation playback unavailable: database missing"), span);
    if (!config || !sessionManager)
        return fail<PlayAnimationResult>(
            ServerError(ServerError::InternalError, "Animation playback unavailable: scheduler dependencies missing"),
            span);
    if (universe < 1 || universe > 63999)
        return fail<PlayAnimationResult>(ServerError(ServerError::InvalidData, "universe must be in [1, 63999]"), span);

    auto result = db->playStoredAnimation(animationId, universe, span);
    if (!result.isSuccess())
        return fail<PlayAnimationResult>(result.getError().value(), span);

    PlayAnimationResult response{result.getValue().value(), std::nullopt};
    if (auto session = sessionManager->getCurrentSession(universe)) {
        response.sessionId = session->getSessionId();
        if (span)
            span->setAttribute("session.id", response.sessionId.value());
    }
    if (span)
        span->setSuccess();
    return Result<PlayAnimationResult>{response};
}

} // namespace creatures::ws
