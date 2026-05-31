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
#include "model/Storyboard.h"
#include "server/config.h"
#include "server/database.h"
#include "server/namespace-stuffs.h"
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

/// REST CRUD for Storyboards — the Console's tap-tile-to-do-thing UI surface.
/// The server is a dumb persistence layer: it stamps id/timestamps, stores the
/// document as-is, and broadcasts cache invalidation on mutate. It does NOT
/// interpret `tiles[].action` — that's the client's vocabulary, evolving
/// independently. Responses bypass oatpp's DTO serializer
/// (ResponseFactory::createResponse + manual JSON) so unknown action keys
/// survive the round trip; the Swagger spec gets `oatpp::String` placeholder
/// with a pointer to creature-console/docs/storyboard-server-contract.md.
class StoryboardController : public oatpp::web::server::api::ApiController,
                             public HttpResponseHelpers<StoryboardController> {
  public:
    StoryboardController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)) : ApiController(objectMapper) {}

    static std::shared_ptr<StoryboardController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                              objectMapper)) {
        return std::make_shared<StoryboardController>(objectMapper);
    }

  private:
    static int64_t nowMillis() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    /// Build the canonical storyboard JSON from raw client input: parse with
    /// nlohmann (lenient — extras are silently kept, the load-bearing
    /// opaque-action property), then stamp the server-managed fields on top.
    /// The result is what `parseStoryboardJson` expects.
    static nlohmann::json buildStoryboardJsonForUpsert(const std::string &rawBody, const std::string &id,
                                                       int64_t createdAt, int64_t updatedAt) {
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

    /// Send a Storyboard back as raw JSON with application/json content type.
    /// We bypass createDtoResponse + StoryboardDto because routing through the
    /// oatpp serializer would silently strip unknown keys inside tile.action,
    /// breaking the forward-compat contract.
    std::shared_ptr<OutgoingResponse> jsonResponse(const Status &status, const nlohmann::json &body) {
        const auto bodyStr = body.dump();
        auto response = oatpp::web::protocol::http::outgoing::ResponseFactory::createResponse(
            status, oatpp::String(bodyStr.c_str()));
        response->putHeader("Content-Type", "application/json; charset=utf-8");
        return response;
    }

  public:
    ENDPOINT_INFO(listStoryboards) {
        info->summary = "List all storyboards (newest first by updated_at)";
        info->description = "Returns {count, items: [Storyboard...]}. Each storyboard's `tiles[].action` is preserved "
                            "verbatim — see creature-console/docs/storyboard-server-contract.md for the action shapes.";
        info->addTag("Storyboards");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/storyboard", listStoryboards, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/storyboard", "GET", "api/v1/storyboard", "listStoryboards",
                           "StoryboardController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               auto opSpan = creatures::observability->createChildOperationSpan(
                                   "StoryboardController.listStoryboards", span);
                               auto result = creatures::db->listStoryboards(opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               const auto storyboards = result.getValue().value();
                               nlohmann::json items = nlohmann::json::array();
                               for (const auto &s : storyboards) {
                                   items.push_back(creatures::storyboardToJson(s));
                               }
                               nlohmann::json envelope;
                               envelope["count"] = items.size();
                               envelope["items"] = items;
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(Status::CODE_200, envelope);
                           });
    }

    ENDPOINT_INFO(getStoryboard) {
        info->summary = "Fetch one storyboard by id";
        info->addTag("Storyboards");
        info->pathParams["storyboardId"].description = "Storyboard UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/storyboard/{storyboardId}", getStoryboard, PATH(String, storyboardId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/v1/storyboard/{storyboardId}", "GET", "api/v1/storyboard/{storyboardId}", "getStoryboard",
            "StoryboardController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!storyboardId || !isUuidShape(std::string(*storyboardId))) {
                    return bailHttp(span, Status::CODE_400, "storyboardId must be a UUID");
                }
                if (span)
                    span->setAttribute("storyboard.id", std::string(*storyboardId));
                auto opSpan =
                    creatures::observability->createChildOperationSpan("StoryboardController.getStoryboard", span);
                auto result = creatures::db->getStoryboard(std::string(*storyboardId), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(Status::CODE_200, creatures::storyboardToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(createStoryboard) {
        info->summary = "Create a new storyboard";
        info->description = "Server generates the storyboard's UUID and stamps created_at + updated_at. Any "
                            "client-supplied `id`/`created_at`/`updated_at` is ignored. `tiles[].action` is stored "
                            "verbatim — see creature-console/docs/storyboard-server-contract.md.";
        info->addTag("Storyboards");
        info->addResponse<oatpp::String>(Status::CODE_201, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/storyboard", createStoryboard, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/storyboard", "POST", "api/v1/storyboard", "createStoryboard", "StoryboardController", request,
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
                    parsed = buildStoryboardJsonForUpsert(std::string(*body), id, now, now);
                } catch (const nlohmann::json::exception &e) {
                    return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                } catch (const std::exception &e) {
                    return bailHttp(span, Status::CODE_400, e.what());
                }

                auto opSpan =
                    creatures::observability->createChildOperationSpan("StoryboardController.createStoryboard", span);

                // Field-level validation (caps, tile shape, UUID-shaped ids).
                auto parseResult = creatures::Database::parseStoryboardJson(parsed, opSpan);
                if (!parseResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                }

                auto result = creatures::db->upsertStoryboard(parsed.dump(), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, creatures::CacheType::StoryboardList);
                if (span) {
                    span->setAttribute("storyboard.id", result.getValue().value().id);
                    span->setHttpStatus(201);
                }
                return jsonResponse(Status::CODE_201, creatures::storyboardToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(updateStoryboard) {
        info->summary = "Update (replace) an existing storyboard";
        info->description = "Body replaces the storyboard's title/notes/tiles; id comes from the URL. created_at is "
                            "preserved from the existing record; updated_at gets bumped to now. Returns 404 if no "
                            "storyboard with that id exists.";
        info->addTag("Storyboards");
        info->pathParams["storyboardId"].description = "Storyboard UUID";
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("PUT", "api/v1/storyboard/{storyboardId}", updateStoryboard, PATH(String, storyboardId),
             BODY_STRING(String, body), REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "PUT /api/v1/storyboard/{storyboardId}", "PUT", "api/v1/storyboard/{storyboardId}", "updateStoryboard",
            "StoryboardController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!storyboardId || !isUuidShape(std::string(*storyboardId))) {
                    return bailHttp(span, Status::CODE_400, "storyboardId must be a UUID");
                }
                if (span)
                    span->setAttribute("storyboard.id", std::string(*storyboardId));
                if (!body) {
                    return bailHttp(span, Status::CODE_400, "Request body is required");
                }
                if (span)
                    span->setAttribute("request.body_size", static_cast<int64_t>(body->size()));

                auto opSpan =
                    creatures::observability->createChildOperationSpan("StoryboardController.updateStoryboard", span);

                // Must exist — PUT replaces, not creates-via-id.
                auto existing = creatures::db->getStoryboard(std::string(*storyboardId), opSpan);
                if (!existing.isSuccess()) {
                    return bailFromServerError(span, existing.getError().value());
                }
                const auto createdAt = existing.getValue().value().created_at;

                nlohmann::json parsed;
                try {
                    parsed = buildStoryboardJsonForUpsert(std::string(*body), std::string(*storyboardId), createdAt,
                                                          nowMillis());
                } catch (const nlohmann::json::exception &e) {
                    return bailHttp(span, Status::CODE_400, fmt::format("Invalid JSON: {}", e.what()));
                } catch (const std::exception &e) {
                    return bailHttp(span, Status::CODE_400, e.what());
                }

                auto parseResult = creatures::Database::parseStoryboardJson(parsed, opSpan);
                if (!parseResult.isSuccess()) {
                    return bailHttp(span, Status::CODE_400, parseResult.getError()->getMessage());
                }

                auto result = creatures::db->upsertStoryboard(parsed.dump(), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, creatures::CacheType::StoryboardList);
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(Status::CODE_200, creatures::storyboardToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(deleteStoryboard) {
        info->summary = "Delete a storyboard";
        info->addTag("Storyboards");
        info->pathParams["storyboardId"].description = "Storyboard UUID";
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("DELETE", "api/v1/storyboard/{storyboardId}", deleteStoryboard, PATH(String, storyboardId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "DELETE /api/v1/storyboard/{storyboardId}", "DELETE", "api/v1/storyboard/{storyboardId}",
            "deleteStoryboard", "StoryboardController", request,
            [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                if (!storyboardId || !isUuidShape(std::string(*storyboardId))) {
                    return bailHttp(span, Status::CODE_400, "storyboardId must be a UUID");
                }
                if (span)
                    span->setAttribute("storyboard.id", std::string(*storyboardId));
                auto opSpan =
                    creatures::observability->createChildOperationSpan("StoryboardController.deleteStoryboard", span);
                auto result = creatures::db->deleteStoryboard(std::string(*storyboardId), opSpan);
                if (!result.isSuccess()) {
                    return bailFromServerError(span, result.getError().value());
                }
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, creatures::CacheType::StoryboardList);
                return okStatus(span, Status::CODE_200, "Storyboard deleted");
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
