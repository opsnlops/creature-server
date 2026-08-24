#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "oatpp/core/Types.hpp"
#include "oatpp/web/protocol/http/Http.hpp"
#include "oatpp/web/server/AsyncHttpConnectionHandler.hpp"

#include <cstdint>
#include <nlohmann/json.hpp>

#include "api/CreatureRequests.h"
#include "api/CreatureResponse.h"
#include "api/JsonResponse.h"
#include "server/database.h"

#include "util/JsonParser.h"

#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/service/CreatureService.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class CreatureController : public oatpp::web::server::api::ApiController,
                           public HttpResponseHelpers<CreatureController> {
  public:
    CreatureController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    CreatureService m_creatureService;

  public:
    static std::shared_ptr<CreatureController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                            objectMapper)) {
        return std::make_shared<CreatureController>(objectMapper);
    }

    ENDPOINT_INFO(getAllCreatures) {
        info->summary = "Get all of the creatures";
        info->addTag("Creatures");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/creature", getAllCreatures, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/creature", "GET", "api/v1/creature", "getAllCreatures", "CreatureController",
                           request, [&](const auto &span) {
                               const auto result = m_creatureService.getAllCreatures(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto items = result.getValue().value();
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::listResponseToJson(items, api::creatureResponseToJson));
                           });
    }

    ENDPOINT_INFO(getCreature) {
        info->summary = "Get one creature by id";
        info->addTag("Creatures");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");

        info->pathParams["creatureId"].description = "Creature ID in the form of an UUID";
    }
    ENDPOINT("GET", "api/v1/creature/{creatureId}", getCreature, PATH(String, creatureId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/creature/{creatureId}", "GET", "api/v1/creature/" + std::string(creatureId),
                           "getCreature", "CreatureController", request, [&](const auto &span) {
                               if (!creatureId || !isUuidShape(std::string(creatureId))) {
                                   return bailHttp(span, Status::CODE_400, "creatureId must be a UUID");
                               }
                               if (span)
                                   span->setAttribute("creature.id", std::string(creatureId));
                               const auto result = m_creatureService.getCreature(std::string(creatureId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200,
                                                   api::creatureResponseToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(exportCreature) {
        info->summary = "Export a creature's raw stored JSON config (disaster recovery)";
        info->description =
            "Returns the creature's configuration exactly as stored in the database, with Mongo's internal "
            "_id stripped, so it can be dropped back onto a replaced controller's JSON file or re-POSTed to "
            "/api/v1/creature. Bypasses the DTO serializer so every stored field survives the round trip — "
            "use this to recover a creature config when its controller's Pi is lost.";
        info->addTag("Creatures");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");

        info->pathParams["creatureId"].description = "Creature ID in the form of an UUID";
    }
    ENDPOINT("GET", "api/v1/creature/{creatureId}/export", exportCreature, PATH(String, creatureId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/creature/{creatureId}/export", "GET",
                           "api/v1/creature/" + std::string(creatureId) + "/export", "exportCreature",
                           "CreatureController", request, [&](const auto &span) -> std::shared_ptr<OutgoingResponse> {
                               if (!creatureId || !isUuidShape(std::string(creatureId))) {
                                   return bailHttp(span, Status::CODE_400, "creatureId must be a UUID");
                               }
                               if (span)
                                   span->setAttribute("creature.id", std::string(creatureId));
                               auto opSpan = creatures::observability
                                                 ? creatures::observability->createChildOperationSpan(
                                                       "CreatureController.exportCreature", span)
                                                 : nullptr;
                               auto result = creatures::db->getCreatureJson(std::string(creatureId), opSpan);
                               if (!result.isSuccess()) {
                                   return bailFromServerError(span, result.getError().value());
                               }
                               // Strip Mongo's internal _id so the export matches the original controller JSON
                               // and POSTs straight back to /api/v1/creature.
                               auto creatureJson = result.getValue().value();
                               creatureJson.erase("_id");
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200, creatureJson);
                           });
    }

    ENDPOINT_INFO(upsertCreature) {
        info->summary = "Upload or update a creature's JSON configuration";
        info->description =
            "Accepts raw creature JSON and upserts it to the database. "
            "All required fields must be present in the JSON (id, name, channel_offset, audio_channel, mouth_slot).";
        info->addTag("Creatures");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_413, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/creature", upsertCreature, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("Upserting creature via POST /api/v1/creature");
        return runEndpoint("POST /api/v1/creature", "POST", "api/v1/creature", "upsertCreature", "CreatureController",
                           request, [&](const auto &span) {
                               const auto creatureConfig =
                                   readRequestBodyLimited(request, MAX_CREATURE_REQUEST_BODY_BYTES, span);
                               const auto result = m_creatureService.upsertCreature(creatureConfig, span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               const auto response = result.getValue().value();
                               if (span) {
                                   span->setAttribute("creature.id", response.creature.id);
                                   span->setAttribute("creature.name", response.creature.name);
                                   span->setHttpStatus(200);
                               }
                               // CreatureService.upsertCreature goes through storage::publishCreature,
                               // which fires the Creature invalidation on success (issue #11 PR #21).
                               return jsonResponse(span, Status::CODE_200, api::creatureResponseToJson(response));
                           });
    }

    ENDPOINT_INFO(validateCreatureConfig) {
        info->summary = "Validate a creature configuration payload";
        info->addTag("Creatures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_413, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/creature/validate", validateCreatureConfig,
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "POST /api/v1/creature/validate", "POST", "api/v1/creature/validate", "validateCreatureConfig",
            "CreatureController", request, [&](const auto &span) {
                const auto creatureConfig = readRequestBodyLimited(request, MAX_CREATURE_REQUEST_BODY_BYTES, span);
                const auto result = m_creatureService.validateCreatureConfig(creatureConfig, span);
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::creatureConfigValidationResponseToJson(result));
            });
    }

    ENDPOINT_INFO(setIdleEnabled) {
        info->summary = "Enable or disable idle loop for a creature (runtime-only)";
        info->addTag("Creatures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
    }
    ENDPOINT("PATCH", "api/v1/creature/{creatureId}/idle", setIdleEnabled, PATH(String, creatureId),
             REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "PATCH /api/v1/creature/{creatureId}/idle", "PATCH", "api/v1/creature/" + std::string(creatureId) + "/idle",
            "setIdleEnabled", "CreatureController", request, [&](const auto &span) {
                if (!creatureId || !isUuidShape(std::string(creatureId))) {
                    return bailHttp(span, Status::CODE_400, "creatureId must be a UUID");
                }
                if (span)
                    span->setAttribute("creature.id", std::string(creatureId));
                const auto body = readRequestBodyLimited(request, api::MAX_IDLE_TOGGLE_REQUEST_BODY_BYTES, span);
                const auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                                      "CreatureController.parseIdleToggleRequest", span)
                                                                : nullptr;
                const auto jsonResult = JsonParser::parseApiJsonString(body, "idle toggle request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto toggleResult = api::idleToggleRequestFromJson(jsonResult.getValue().value());
                if (!toggleResult.isSuccess()) {
                    const auto error = toggleResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidIdleToggleEnvelope", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto result =
                    m_creatureService.setIdleEnabled(std::string(creatureId), toggleResult.getValue()->enabled, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::creatureResponseToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(registerCreature) {
        info->summary = "Register a creature with its universe assignment";
        info->description =
            "Called by controllers when they start up to register a creature and its current universe. "
            "The creature config from the controller's JSON file is the source of truth and will be upserted "
            "to the database. The universe assignment is stored in runtime memory only.";
        info->addTag("Creatures");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_413, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/creature/register", registerCreature, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        debug("----> Controller registering creature with universe assignment");
        return runEndpoint(
            "POST /api/v1/creature/register", "POST", "api/v1/creature/register", "registerCreature",
            "CreatureController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, MAX_CREATURE_REQUEST_BODY_BYTES, span);

                debug("Raw request body size: {} bytes", body.size());

                const auto parseSpan = creatures::observability
                                           ? creatures::observability->createChildOperationSpan(
                                                 "CreatureController.parseRegistrationRequest", span)
                                           : nullptr;
                const auto jsonResult =
                    JsonParser::parseApiJsonString(body, "creature registration request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto registrationResult = api::registerCreatureRequestFromJson(jsonResult.getValue().value());
                if (!registrationResult.isSuccess()) {
                    const auto error = registrationResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidRegistrationEnvelope", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto registration = registrationResult.getValue().value();

                if (span) {
                    span->setAttribute("universe", static_cast<int64_t>(registration.universe));
                    span->setAttribute("creature.config.size",
                                       static_cast<int64_t>(registration.creatureConfig.length()));
                }

                const auto result =
                    m_creatureService.registerCreature(registration.creatureConfig, registration.universe, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                const auto response = result.getValue().value();

                if (span) {
                    span->setAttribute("creature.id", response.creature.id);
                    span->setAttribute("creature.name", response.creature.name);
                    span->setHttpStatus(200);
                }

                // CreatureService.registerCreature → ... → storage::publishCreature
                // fires the Creature invalidation on success (issue #11 PR #21).

                return jsonResponse(span, Status::CODE_200, api::creatureResponseToJson(response));
            });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
