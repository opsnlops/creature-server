#pragma once

#include <chrono>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/outgoing/ResponseFactory.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "model/CacheInvalidation.h"
#include "model/Stage.h"
#include "server/config.h"
#include "server/database.h"
#include "server/jobs/JobManager.h"
#include "server/jobs/JobWorker.h"
#include "server/namespace-stuffs.h"
#include "server/storage/Storage.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/JobCreatedDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<jobs::JobManager> jobManager;
extern std::shared_ptr<jobs::JobWorker> jobWorker;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures::ws {

/// REST CRUD for Stages — where each creature sits and which way it faces
/// (issue #119). The gaze layer reads this to aim heads at whoever is
/// speaking, and the Console's spatial-audio renderer reads the same document
/// to position its emitters, so there is exactly one source of truth for the
/// physical arrangement instead of two that drift.
///
/// Coordinates are metres relative to the listener, who sits at the ORIGIN
/// facing -Z. See model/Stage.h for the full frame definition.
///
/// The server validates the geometry it uses (creature_id, x/y/z, yaw) and
/// treats everything else as opaque: per-placement extras like gain and
/// muted, and the whole console-owned `audio` block, are stored and returned
/// verbatim. Responses therefore bypass oatpp's DTO serializer
/// (ResponseFactory::createResponse + manual JSON), since routing through it
/// would silently strip console-owned keys the model intentionally preserves.
class StageController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<StageController> {
  public:
    StageController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<StageController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) {
        return std::make_shared<StageController>(objectMapper);
    }

  private:
    static int64_t nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// Build the canonical stage JSON from raw client input: parse with
    /// nlohmann (lenient — console-owned extras are silently kept, which is
    /// the load-bearing property), then stamp the server-managed fields on top.
    /// The result is what `parseStageJson` expects.
    static nlohmann::json buildStageJsonForUpsert(const std::string &rawBody, const std::string &id, int64_t createdAt,
                                                  int64_t updatedAt) {
        auto parsed = nlohmann::json::parse(rawBody); // throws on bad JSON; caller catches.
        if (!parsed.is_object()) {
            throw std::runtime_error("request body must be a JSON object");
        }
        // Stamp the server-managed fields. Any client-supplied values get overwritten.
        // The contract says ignore client-side id/created_at/updated_at — overwriting
        // is the cleanest way to enforce that without rejecting the request.
        parsed["id"] = id;
        parsed["created_at"] = createdAt;
        parsed["updated_at"] = updatedAt;
        return parsed;
    }

    /// Send a Stage back as raw JSON with application/json content type.
    /// We bypass createDtoResponse because routing through the
    /// oatpp serializer would strip the console-owned keys we promised to
    /// preserve.
    std::shared_ptr<OutgoingResponse> jsonResponse(const Status &status, const nlohmann::json &body) {
        const auto bodyStr = body.dump();
        auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
            status, oatpp::String(bodyStr.c_str()));
        response->putHeader("Content-Type", "application/json; charset=utf-8");
        return response;
    }

  public:
    ENDPOINT_INFO(listStages) {
        info->summary = "List all stages (newest first by updated_at)";
        info->description = "Returns {count, items: [Stage...]}, newest first. Console-owned fields on each stage "
                            "are preserved verbatim.";
        info->addTag("Stages");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/stage", listStages, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/stage", "GET", "api/v1/stage", "listStages", "StageController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "StageController.listStages", span);
                               auto result = creatures::db->listStages(opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               const auto stages = result.getValue().value();
                               nlohmann::json items = nlohmann::json::array();
                               for (const auto &s : stages) {
                                   items.push_back(creatures::stageToJson(s));
                               }
                               nlohmann::json envelope;
                               envelope["count"] = items.size();
                               envelope["items"] = items;
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(Status::CODE_200, envelope);
                           });
    }

    ENDPOINT_INFO(getStage) {
        info->summary = "Fetch one stage by id";
        info->addTag("Stages");
        info->pathParams["stageId"].description = "Stage UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/stage/{stageId}", getStage, PATH(String, stageId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/stage/{stageId}", "GET", "api/v1/stage/{stageId}", "getStage",
                           "StageController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!stageId || !isUuidShape(std::string(*stageId))) {
                                   return bailHttp(span, Status::CODE_400, "stageId must be a UUID");
                               }
                               if (span)
                                   span->setAttribute("stage.id", std::string(*stageId));
                               auto opSpan =
                                   creatures::observability->createChildOperationSpan("StageController.getStage", span);
                               auto result = creatures::db->getStage(std::string(*stageId), opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(Status::CODE_200, creatures::stageToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(createStage) {
        info->summary = "Create a new stage";
        info->description = "Server generates the stage's UUID and stamps created_at + updated_at. Any "
                            "client-supplied `id`/`created_at`/`updated_at` is ignored. Placements need a "
                            "creature_id plus x/y/z in metres (listener at the origin, +/-5 m) and yaw in degrees.";
        info->addTag("Stages");
        info->addResponse<oatpp::String>(Status::CODE_201, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/stage", createStage, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("POST /api/v1/stage", "POST", "api/v1/stage", "createStage", "StageController", request,
                           [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!body) {
                                   return bailHttp(span, Status::CODE_400, "Request body is required");
                               }
                               if (span)
                                   span->setAttribute("request.body_size", static_cast<int64_t>(body->size()));

                               const auto now = nowMillis();
                               const auto id = util::generateUUID();
                               nlohmann::json parsed;
                               try {
                                   parsed = buildStageJsonForUpsert(std::string(*body), id, now, now);
                               } catch (const nlohmann::json::exception &e) {
                                   return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                               } catch (const std::exception &e) {
                                   return bailHttp(span, Status::CODE_400, e.what());
                               }

                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "StageController.createStage", span);

                               // Field-level validation (caps, tile shape, UUID-shaped ids).
                               auto parseResult = creatures::Database::parseStageJson(parsed, opSpan);
                               if (!parseResult.isSuccess()) {
                                   return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                               }

                               auto result = creatures::storage::publishStage(parsed.dump(), opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               if (span) {
                                   span->setAttribute("stage.id", result.getValue().value().id);
                                   span->setHttpStatus(201);
                               }
                               return jsonResponse(Status::CODE_201, creatures::stageToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(updateStage) {
        info->summary = "Update (replace) an existing stage";
        info->description = "Body replaces the stage's title/notes/placements/audio; id comes from the URL. "
                            "created_at is preserved from the existing record; updated_at gets bumped to now, which "
                            "is what marks animations rendered against this stage as stale. Returns 404 if no stage "
                            "with that id exists.";
        info->addTag("Stages");
        info->pathParams["stageId"].description = "Stage UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("PUT", "api/v1/stage/{stageId}", updateStage, PATH(String, stageId), BODY_STRING(String, body),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "PUT /api/v1/stage/{stageId}", "PUT", "api/v1/stage/{stageId}", "updateStage", "StageController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!stageId || !isUuidShape(std::string(*stageId))) {
                    return bailHttp(span, Status::CODE_400, "stageId must be a UUID");
                }
                if (span)
                    span->setAttribute("stage.id", std::string(*stageId));
                if (!body) {
                    return bailHttp(span, Status::CODE_400, "Request body is required");
                }
                if (span)
                    span->setAttribute("request.body_size", static_cast<int64_t>(body->size()));

                auto opSpan = creatures::observability->createChildOperationSpan("StageController.updateStage", span);

                // Must exist — PUT replaces, not creates-via-id.
                auto existing = creatures::db->getStage(std::string(*stageId), opSpan);
                if (!existing.isSuccess()) {
                    return bailFromServerError(span, existing.getError().value());
                }
                const auto createdAt = existing.getValue().value().created_at;

                nlohmann::json parsed;
                try {
                    parsed = buildStageJsonForUpsert(std::string(*body), std::string(*stageId), createdAt, nowMillis());
                } catch (const nlohmann::json::exception &e) {
                    return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                } catch (const std::exception &e) {
                    return bailHttp(span, Status::CODE_400, e.what());
                }

                auto parseResult = creatures::Database::parseStageJson(parsed, opSpan);
                if (!parseResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                }

                auto result = creatures::storage::publishStage(parsed.dump(), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(Status::CODE_200, creatures::stageToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(listStageAnimations) {
        info->summary = "List the animations rendered against this stage";
        info->description =
            "Returns {count, stale_count, items:[{animation_id, title, source_script_id, source_stage_updated_at, "
            "stale}]}, most out-of-date first. An animation is `stale` when it was rendered against an older version "
            "of this stage than the one currently stored — move a creature and every animation built on that stage "
            "becomes stale until it is re-rendered.";
        info->addTag("Stages");
        info->pathParams["stageId"].description = "Stage UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/stage/{stageId}/animations", listStageAnimations, PATH(String, stageId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/stage/{stageId}/animations", "GET", "api/v1/stage/{stageId}/animations", "listStageAnimations",
            "StageController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!stageId || !isUuidShape(std::string(*stageId))) {
                    return bailHttp(span, Status::CODE_400, "stageId must be a UUID");
                }
                if (span)
                    span->setAttribute("stage.id", std::string(*stageId));
                auto opSpan =
                    creatures::observability->createChildOperationSpan("StageController.listStageAnimations", span);

                // Load the stage first: its current updated_at is what
                // staleness is measured against, and a missing stage should be
                // a 404 rather than an empty list.
                auto stageResult = creatures::db->getStage(std::string(*stageId), opSpan);
                if (!stageResult.isSuccess()) {
                    return bailFromServerError(span, stageResult.getError().value());
                }
                const auto stage = stageResult.getValue().value();

                auto result = creatures::db->listAnimationsBySourceStageId(stage.id, stage.updated_at, opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                const auto refs = result.getValue().value();

                nlohmann::json items = nlohmann::json::array();
                std::size_t staleCount = 0;
                for (const auto &ref : refs) {
                    items.push_back({{"animation_id", ref.animation_id},
                                     {"title", ref.title},
                                     {"source_script_id", ref.source_script_id},
                                     {"source_stage_updated_at", ref.source_stage_updated_at},
                                     {"stale", ref.stale}});
                    if (ref.stale) {
                        ++staleCount;
                    }
                }

                nlohmann::json envelope;
                envelope["count"] = items.size();
                envelope["stale_count"] = staleCount;
                envelope["stage_updated_at"] = stage.updated_at;
                envelope["items"] = items;
                if (span) {
                    span->setAttribute("animations.count", static_cast<int64_t>(items.size()));
                    span->setAttribute("animations.stale_count", static_cast<int64_t>(staleCount));
                    span->setHttpStatus(200);
                }
                return jsonResponse(Status::CODE_200, envelope);
            });
    }

    ENDPOINT_INFO(rerenderStage) {
        info->summary = "Re-render the animations built on this stage against its current geometry";
        info->description =
            "Rebuilds each animation's MOTION only — the existing audio is reused untouched, and no speech is "
            "regenerated. That is a correctness requirement, not an optimisation: the dialogue model is "
            "nondeterministic and exposes no seed, so regenerating audio because a creature moved would change the "
            "performance itself.\n\n"
            "Because each animation records the loops its creatures drew and the seed its gaze timing came from, a "
            "re-render against an UNCHANGED stage reproduces the animation exactly, and a re-render after moving a "
            "creature changes only the head-aiming bytes.\n\n"
            "Body: {\"stale_only\": true} to skip animations already current with this stage. Returns 202 with a "
            "job_id; listen for job-progress and job-complete.";
        info->addTag("Stages");
        info->pathParams["stageId"].description = "Stage UUID";
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/stage/{stageId}/rerender", rerenderStage, PATH(String, stageId), BODY_STRING(String, body),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/stage/{stageId}/rerender", "POST", "api/v1/stage/{stageId}/rerender", "rerenderStage",
            "StageController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!stageId || !isUuidShape(std::string(*stageId))) {
                    return bailHttp(span, Status::CODE_400, "stageId must be a UUID");
                }
                if (span)
                    span->setAttribute("stage.id", std::string(*stageId));

                // Body is optional; an absent or empty one means "everything".
                bool staleOnly = false;
                if (body && !body->empty()) {
                    try {
                        const auto parsed = nlohmann::json::parse(std::string(*body));
                        if (parsed.is_object()) {
                            staleOnly = parsed.value("stale_only", false);
                        }
                    } catch (const nlohmann::json::exception &e) {
                        return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                    }
                }

                auto opSpan = creatures::observability->createChildOperationSpan("StageController.rerenderStage", span);
                auto stageResult = creatures::db->getStage(std::string(*stageId), opSpan);
                if (!stageResult.isSuccess()) {
                    return bailFromServerError(span, stageResult.getError().value());
                }
                const auto stage = stageResult.getValue().value();

                auto listResult = creatures::db->listAnimationsBySourceStageId(stage.id, stage.updated_at, opSpan);
                if (!listResult.isSuccess()) {
                    return bailFromServerError(span, listResult.getError().value());
                }

                // Bind to a named local: getValue() hands back the optional BY
                // VALUE, so iterating .value() directly would walk a vector
                // that died at the end of the expression.
                const auto refs = listResult.getValue().value();

                nlohmann::json animationIds = nlohmann::json::array();
                for (const auto &ref : refs) {
                    if (staleOnly && !ref.stale) {
                        continue;
                    }
                    animationIds.push_back(ref.animation_id);
                }
                if (animationIds.empty()) {
                    return okStatus(span, Status::CODE_200,
                                    staleOnly ? "Nothing to do — every animation on this stage is up to date"
                                              : "No animations are rendered against this stage");
                }

                nlohmann::json details;
                details["animation_ids"] = animationIds;
                details["stage_id"] = stage.id;

                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::StageRerender, details.dump(), span);
                creatures::jobWorker->queueJob(jobId);

                auto response = JobCreatedDto::createShared();
                response->job_id = jobId.c_str();
                response->job_type = "stage-rerender";
                response->message = fmt::format("Re-rendering {} animation(s) against stage '{}'. No audio is "
                                                "regenerated. Listen for job-progress and job-complete.",
                                                animationIds.size(), stage.title)
                                        .c_str();
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setAttribute("rerender.animation_count", static_cast<int64_t>(animationIds.size()));
                    span->setHttpStatus(202);
                }
                return createDtoResponse(Status::CODE_202, response);
            });
    }

    ENDPOINT_INFO(rerenderAnimation) {
        info->summary = "Re-render one animation's motion against a stage";
        info->description =
            "Motion only — the existing audio is reused untouched and no speech is regenerated.\n\n"
            "Body: {\"stage_id\": \"...\"} to aim against a different stage than the one this animation was "
            "rendered with; omit it to rebuild against its own recorded stage. Note this OVERWRITES the animation in "
            "place. To keep both a mainstage and a travel rendition, render the script twice with different "
            "stage_ids instead — animations are keyed by (script, stage), so the two coexist.\n\n"
            "Returns 202 with a job_id.";
        info->addTag("Stages");
        info->pathParams["animationId"].description = "Animation UUID";
        info->addResponse<Object<JobCreatedDto>>(Status::CODE_202, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/animation/{animationId}/rerender", rerenderAnimation, PATH(String, animationId),
             BODY_STRING(String, body), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/animation/{animationId}/rerender", "POST", "api/v1/animation/{animationId}/rerender",
            "rerenderAnimation", "StageController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!animationId || !isUuidShape(std::string(*animationId))) {
                    return bailHttp(span, Status::CODE_400, "animationId must be a UUID");
                }
                if (span)
                    span->setAttribute("animation.id", std::string(*animationId));

                std::string targetStageId;
                if (body && !body->empty()) {
                    try {
                        const auto parsed = nlohmann::json::parse(std::string(*body));
                        if (parsed.is_object()) {
                            targetStageId = parsed.value("stage_id", std::string{});
                        }
                    } catch (const nlohmann::json::exception &e) {
                        return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                    }
                }
                if (!targetStageId.empty() && !isUuidShape(targetStageId)) {
                    return bailHttp(span, Status::CODE_400, "stage_id must be a UUID");
                }

                nlohmann::json details;
                details["animation_ids"] = nlohmann::json::array({std::string(*animationId)});
                details["stage_id"] = targetStageId;

                const std::string jobId =
                    creatures::jobManager->createJob(creatures::jobs::JobType::StageRerender, details.dump(), span);
                creatures::jobWorker->queueJob(jobId);

                auto response = JobCreatedDto::createShared();
                response->job_id = jobId.c_str();
                response->job_type = "stage-rerender";
                response->message = "Re-rendering animation motion. No audio is regenerated. Listen for "
                                    "job-progress and job-complete.";
                if (span) {
                    span->setAttribute("job.id", jobId);
                    span->setHttpStatus(202);
                }
                return createDtoResponse(Status::CODE_202, response);
            });
    }

    ENDPOINT_INFO(deleteStage) {
        info->summary = "Delete a stage";
        info->addTag("Stages");
        info->pathParams["stageId"].description = "Stage UUID";
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/stage/{stageId}", deleteStage, PATH(String, stageId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("DELETE /api/v1/stage/{stageId}", "DELETE", "api/v1/stage/{stageId}", "deleteStage",
                           "StageController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!stageId || !isUuidShape(std::string(*stageId))) {
                                   return bailHttp(span, Status::CODE_400, "stageId must be a UUID");
                               }
                               if (span)
                                   span->setAttribute("stage.id", std::string(*stageId));
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "StageController.deleteStage", span);
                               auto result = creatures::storage::deleteStage(std::string(*stageId), opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               return okStatus(span, Status::CODE_200, "Stage deleted");
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
