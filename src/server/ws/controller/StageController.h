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
#include "server/namespace-stuffs.h"
#include "server/storage/Storage.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/StatusDto.h"
#include "util/uuidUtils.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
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
/// would silently strip any key StageDto doesn't model.
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
    /// We bypass createDtoResponse + StageDto because routing through the
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
