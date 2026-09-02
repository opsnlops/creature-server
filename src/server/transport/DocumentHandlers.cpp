#include "server/transport/DocumentHandlers.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "api/JobResponses.h"
#include "api/JsonResponse.h"
#include "model/Stage.h"
#include "model/Storyboard.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/storage/Storage.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::transport {
namespace {

int64_t nowMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

PreparedResponse errorResponse(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    const auto code = serverErrorToStatusCode(error.getCode());
    recordSpanError(span, error.getMessage(), "DocumentOperationFailed", error.getCode());
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, error.getMessage()))));
}

PreparedResponse statusResponse(const int code, std::string message, const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        if (code >= 400) {
            span->setAttribute("error.type", "InvalidRequest");
            span->setAttribute("error.code", static_cast<int64_t>(code));
            span->setError(message);
        } else {
            span->setSuccess();
        }
    }
    return PreparedResponse::json(
        code, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(code, std::move(message)))));
}

std::shared_ptr<OperationSpan> child(const std::string &name, const std::shared_ptr<OperationSpan> &span) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, span) : nullptr;
}

nlohmann::json canonicalDocument(const std::string &body, const std::string &id, const int64_t createdAt,
                                 const int64_t updatedAt) {
    auto parsed = nlohmann::json::parse(body);
    if (!parsed.is_object()) {
        throw std::runtime_error("request body must be a JSON object");
    }
    parsed["id"] = id;
    parsed["created_at"] = createdAt;
    parsed["updated_at"] = updatedAt;
    return parsed;
}

template <typename Builder>
PreparedResponse parseBody(const std::string &body, const std::shared_ptr<OperationSpan> &span, Builder builder) {
    if (body.empty()) {
        return statusResponse(400, "Request body is required", span);
    }
    try {
        return builder();
    } catch (const nlohmann::json::exception &error) {
        return statusResponse(400, fmt::format("Invalid JSON: {}", error.what()), span);
    } catch (const std::exception &error) {
        return statusResponse(400, error.what(), span);
    }
}

} // namespace

PreparedResponse listStages(const std::shared_ptr<OperationSpan> &span) {
    if (!creatures::db) {
        return statusResponse(500, "Stage database unavailable", span);
    }
    auto operation = child("StageController.listStages", span);
    const auto result = creatures::db->listStages(operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    const auto stages = result.getValue().value();
    nlohmann::json items = nlohmann::json::array();
    for (const auto &stage : stages) {
        items.push_back(stageToJson(stage));
    }
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setAttribute("stages.count", static_cast<int64_t>(items.size()));
        span->setSuccess();
    }
    return PreparedResponse::json(200, nlohmann::json{{"count", items.size()}, {"items", items}}.dump());
}

PreparedResponse getStage(const std::string &stageId, const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        span->setAttribute("stage.id", stageId);
    }
    if (!isUuidShape(stageId)) {
        return statusResponse(400, "stageId must be a UUID", span);
    }
    if (!creatures::db) {
        return statusResponse(500, "Stage database unavailable", span);
    }
    auto operation = child("StageController.getStage", span);
    const auto result = creatures::db->getStage(stageId, operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, stageToJson(result.getValue().value()).dump());
}

PreparedResponse createStage(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    return parseBody(body, span, [&] {
        const auto now = nowMillis();
        const auto parsed = canonicalDocument(body, util::generateUUID(), now, now);
        auto operation = child("StageController.createStage", span);
        const auto parsedStage = Database::parseStageJson(parsed, operation);
        if (!parsedStage.isSuccess()) {
            return statusResponse(400, parsedStage.getError()->getMessage(), span);
        }
        const auto result = storage::publishStage(parsed.dump(), operation);
        if (!result.isSuccess()) {
            return errorResponse(result.getError().value(), span);
        }
        if (operation) {
            operation->setSuccess();
        }
        if (span) {
            span->setAttribute("stage.id", result.getValue()->id);
            span->setSuccess();
        }
        return PreparedResponse::json(201, stageToJson(result.getValue().value()).dump());
    });
}

PreparedResponse updateStage(const std::string &stageId, const std::string &body,
                             const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        span->setAttribute("stage.id", stageId);
    }
    if (!isUuidShape(stageId)) {
        return statusResponse(400, "stageId must be a UUID", span);
    }
    return parseBody(body, span, [&] {
        auto operation = child("StageController.updateStage", span);
        const auto existing = creatures::db->getStage(stageId, operation);
        if (!existing.isSuccess()) {
            return errorResponse(existing.getError().value(), span);
        }
        const auto parsed = canonicalDocument(body, stageId, existing.getValue()->created_at, nowMillis());
        const auto parsedStage = Database::parseStageJson(parsed, operation);
        if (!parsedStage.isSuccess()) {
            return statusResponse(400, parsedStage.getError()->getMessage(), span);
        }
        const auto result = storage::publishStage(parsed.dump(), operation);
        if (!result.isSuccess()) {
            return errorResponse(result.getError().value(), span);
        }
        if (operation) {
            operation->setSuccess();
        }
        if (span) {
            span->setSuccess();
        }
        return PreparedResponse::json(200, stageToJson(result.getValue().value()).dump());
    });
}

PreparedResponse deleteStage(const std::string &stageId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(stageId)) {
        return statusResponse(400, "stageId must be a UUID", span);
    }
    auto operation = child("StageController.deleteStage", span);
    const auto result = storage::deleteStage(stageId, operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    if (operation) {
        operation->setSuccess();
    }
    return statusResponse(200, "Stage deleted", span);
}

PreparedResponse listStageAnimations(const std::string &stageId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(stageId)) {
        return statusResponse(400, "stageId must be a UUID", span);
    }
    auto operation = child("StageController.listStageAnimations", span);
    const auto stageResult = creatures::db->getStage(stageId, operation);
    if (!stageResult.isSuccess()) {
        return errorResponse(stageResult.getError().value(), span);
    }
    const auto stage = stageResult.getValue().value();
    const auto result = creatures::db->listAnimationsBySourceStageId(stage.id, stage.updated_at, operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    const auto references = result.getValue().value();
    nlohmann::json items = nlohmann::json::array();
    std::size_t staleCount = 0;
    for (const auto &ref : references) {
        items.push_back({{"animation_id", ref.animation_id},
                         {"title", ref.title},
                         {"source_script_id", ref.source_script_id},
                         {"source_stage_updated_at", ref.source_stage_updated_at},
                         {"stale", ref.stale}});
        staleCount += ref.stale ? 1 : 0;
    }
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, nlohmann::json{{"count", items.size()},
                                                      {"stale_count", staleCount},
                                                      {"stage_updated_at", stage.updated_at},
                                                      {"items", items}}
                                           .dump());
}

PreparedResponse rerenderStage(const std::string &stageId, const std::string &body,
                               const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(stageId)) {
        return statusResponse(400, "stageId must be a UUID", span);
    }
    bool staleOnly = false;
    if (!body.empty()) {
        try {
            const auto parsed = nlohmann::json::parse(body);
            if (parsed.is_object()) {
                staleOnly = parsed.value("stale_only", false);
            }
        } catch (const nlohmann::json::exception &error) {
            return statusResponse(400, fmt::format("Invalid JSON: {}", error.what()), span);
        }
    }
    auto operation = child("StageController.rerenderStage", span);
    const auto stageResult = creatures::db->getStage(stageId, operation);
    if (!stageResult.isSuccess()) {
        return errorResponse(stageResult.getError().value(), span);
    }
    const auto stage = stageResult.getValue().value();
    const auto listResult = creatures::db->listAnimationsBySourceStageId(stage.id, stage.updated_at, operation);
    if (!listResult.isSuccess()) {
        return errorResponse(listResult.getError().value(), span);
    }
    const auto references = listResult.getValue().value();
    nlohmann::json animationIds = nlohmann::json::array();
    for (const auto &ref : references) {
        if (!staleOnly || ref.stale) {
            animationIds.push_back(ref.animation_id);
        }
    }
    if (animationIds.empty()) {
        return statusResponse(200,
                              staleOnly ? "Nothing to do — every animation on this stage is up to date"
                                        : "No animations are rendered against this stage",
                              span);
    }
    nlohmann::json details{{"animation_ids", animationIds}, {"stage_id", stage.id}};
    const auto jobId = creatures::jobManager->createJob(jobs::JobType::StageRerender, details.dump());
    creatures::jobWorker->queueJob(jobId);
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setAttribute("job.id", jobId);
        span->setSuccess();
    }
    const api::JobCreatedResponse response{
        jobId, "stage-rerender",
        fmt::format("Re-rendering {} animation(s) against stage '{}'. No audio is regenerated. Listen for "
                    "job-progress and job-complete.",
                    animationIds.size(), stage.title)};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

PreparedResponse rerenderAnimation(const std::string &animationId, const std::string &body,
                                   const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(animationId)) {
        return statusResponse(400, "animationId must be a UUID", span);
    }
    std::string stageId;
    if (!body.empty()) {
        try {
            const auto parsed = nlohmann::json::parse(body);
            if (parsed.is_object()) {
                stageId = parsed.value("stage_id", std::string{});
            }
        } catch (const nlohmann::json::exception &error) {
            return statusResponse(400, fmt::format("Invalid JSON: {}", error.what()), span);
        }
    }
    if (!stageId.empty() && !isUuidShape(stageId)) {
        return statusResponse(400, "stage_id must be a UUID", span);
    }
    nlohmann::json details{{"animation_ids", nlohmann::json::array({animationId})}, {"stage_id", stageId}};
    const auto jobId = creatures::jobManager->createJob(jobs::JobType::StageRerender, details.dump());
    creatures::jobWorker->queueJob(jobId);
    if (span) {
        span->setAttribute("animation.id", animationId);
        span->setAttribute("job.id", jobId);
        span->setSuccess();
    }
    const api::JobCreatedResponse response{
        jobId, "stage-rerender",
        "Re-rendering animation motion. No audio is regenerated. Listen for job-progress and job-complete."};
    return PreparedResponse::json(202, api::jsonToString(api::jobCreatedResponseToJson(response)));
}

PreparedResponse listStoryboards(const std::shared_ptr<OperationSpan> &span) {
    auto operation = child("StoryboardController.listStoryboards", span);
    const auto result = creatures::db->listStoryboards(operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    const auto storyboards = result.getValue().value();
    nlohmann::json items = nlohmann::json::array();
    for (const auto &storyboard : storyboards) {
        items.push_back(storyboardToJson(storyboard));
    }
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, nlohmann::json{{"count", items.size()}, {"items", items}}.dump());
}

PreparedResponse getStoryboard(const std::string &storyboardId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(storyboardId)) {
        return statusResponse(400, "storyboardId must be a UUID", span);
    }
    auto operation = child("StoryboardController.getStoryboard", span);
    const auto result = creatures::db->getStoryboard(storyboardId, operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    if (operation) {
        operation->setSuccess();
    }
    if (span) {
        span->setSuccess();
    }
    return PreparedResponse::json(200, storyboardToJson(result.getValue().value()).dump());
}

PreparedResponse createStoryboard(const std::string &body, const std::shared_ptr<OperationSpan> &span) {
    return parseBody(body, span, [&] {
        const auto now = nowMillis();
        const auto parsed = canonicalDocument(body, util::generateUUID(), now, now);
        auto operation = child("StoryboardController.createStoryboard", span);
        const auto parsedStoryboard = Database::parseStoryboardJson(parsed, operation);
        if (!parsedStoryboard.isSuccess()) {
            return statusResponse(400, parsedStoryboard.getError()->getMessage(), span);
        }
        const auto result = storage::publishStoryboard(parsed.dump(), operation);
        if (!result.isSuccess()) {
            return errorResponse(result.getError().value(), span);
        }
        if (operation) {
            operation->setSuccess();
        }
        if (span) {
            span->setSuccess();
        }
        return PreparedResponse::json(201, storyboardToJson(result.getValue().value()).dump());
    });
}

PreparedResponse updateStoryboard(const std::string &storyboardId, const std::string &body,
                                  const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(storyboardId)) {
        return statusResponse(400, "storyboardId must be a UUID", span);
    }
    return parseBody(body, span, [&] {
        auto operation = child("StoryboardController.updateStoryboard", span);
        const auto existing = creatures::db->getStoryboard(storyboardId, operation);
        if (!existing.isSuccess()) {
            return errorResponse(existing.getError().value(), span);
        }
        const auto parsed = canonicalDocument(body, storyboardId, existing.getValue()->created_at, nowMillis());
        const auto parsedStoryboard = Database::parseStoryboardJson(parsed, operation);
        if (!parsedStoryboard.isSuccess()) {
            return statusResponse(400, parsedStoryboard.getError()->getMessage(), span);
        }
        const auto result = storage::publishStoryboard(parsed.dump(), operation);
        if (!result.isSuccess()) {
            return errorResponse(result.getError().value(), span);
        }
        if (operation) {
            operation->setSuccess();
        }
        if (span) {
            span->setSuccess();
        }
        return PreparedResponse::json(200, storyboardToJson(result.getValue().value()).dump());
    });
}

PreparedResponse deleteStoryboard(const std::string &storyboardId, const std::shared_ptr<OperationSpan> &span) {
    if (!isUuidShape(storyboardId)) {
        return statusResponse(400, "storyboardId must be a UUID", span);
    }
    auto operation = child("StoryboardController.deleteStoryboard", span);
    const auto result = storage::deleteStoryboard(storyboardId, operation);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), span);
    }
    if (operation) {
        operation->setSuccess();
    }
    return statusResponse(200, "Storyboard deleted", span);
}

} // namespace creatures::transport
