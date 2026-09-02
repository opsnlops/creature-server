#include "server/transport/CreatureReadHandlers.h"

#include <cstdint>
#include <utility>

#include "api/CreatureRequests.h"
#include "api/JsonResponse.h"
#include "server/database.h"
#include "server/ws/service/CreatureService.h"
#include "util/JsonParser.h"
#include "util/UuidValidation.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::transport {
namespace {

const char *serverErrorType(const ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::Unauthorized:
        return "Unauthorized";
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

PreparedResponse errorResponse(const ServerError &error, const std::shared_ptr<OperationSpan> &requestSpan) {
    const auto statusCode = serverErrorToStatusCode(error.getCode());
    if (requestSpan) {
        requestSpan->setAttribute("error.type", serverErrorType(error.getCode()));
        requestSpan->setAttribute("error.code", static_cast<int64_t>(statusCode));
        requestSpan->setAttribute("error.message", error.getMessage());
        requestSpan->setAttribute("server.error.code", static_cast<int64_t>(error.getCode()));
        requestSpan->setError(error.getMessage());
    }
    return PreparedResponse::json(statusCode, api::jsonToString(api::statusResponseToJson(
                                                  api::makeStatusResponse(statusCode, error.getMessage()))));
}

} // namespace

PreparedResponse listCreatures(const std::shared_ptr<OperationSpan> &requestSpan, const CreatureListLoader &loader) {
    const auto result = loader(requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }

    const auto creatures = result.getValue().value();
    if (requestSpan) {
        requestSpan->setAttribute("creatures.count", static_cast<int64_t>(creatures.size()));
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200,
                                  api::jsonToString(api::listResponseToJson(creatures, api::creatureResponseToJson)));
}

PreparedResponse getCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &requestSpan,
                             const CreatureLoader &loader) {
    if (requestSpan) {
        requestSpan->setAttribute("creature.id", creatureId);
    }
    if (!isUuidShape(creatureId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "creatureId must be a UUID"), requestSpan);
    }

    const auto result = loader(creatureId, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::creatureResponseToJson(result.getValue().value())));
}

PreparedResponse setCreatureIdle(const creatureId_t &creatureId, const std::string &body,
                                 const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("creature.id", creatureId);
        requestSpan->setAttribute("http.request.body.size", static_cast<int64_t>(body.size()));
    }
    if (!isUuidShape(creatureId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "creatureId must be a UUID"), requestSpan);
    }

    auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                    "CreatureController.parseIdleToggleRequest", requestSpan)
                                              : nullptr;
    const auto jsonResult = JsonParser::parseApiJsonString(body, "idle toggle request", parseSpan);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), requestSpan);
    }
    const auto toggleResult = api::idleToggleRequestFromJson(jsonResult.getValue().value());
    if (!toggleResult.isSuccess()) {
        const auto error = toggleResult.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidIdleToggleEnvelope", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }

    const auto result = creatures::ws::CreatureService::setIdleEnabled(creatureId, toggleResult.getValue()->enabled,
                                                                       nullptr, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::creatureResponseToJson(result.getValue().value())));
}

PreparedResponse upsertCreature(const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    const auto result = creatures::ws::CreatureService::upsertCreature(body, nullptr, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    const auto response = result.getValue().value();
    if (requestSpan) {
        requestSpan->setAttribute("creature.id", response.creature.id);
        requestSpan->setAttribute("creature.name", response.creature.name);
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::creatureResponseToJson(response)));
}

PreparedResponse validateCreature(const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    const auto result = creatures::ws::CreatureService::validateCreatureConfig(body, nullptr, requestSpan);
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::creatureConfigValidationResponseToJson(result)));
}

PreparedResponse registerCreature(const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    auto parseSpan = creatures::observability ? creatures::observability->createChildOperationSpan(
                                                    "CreatureController.parseRegistrationRequest", requestSpan)
                                              : nullptr;
    const auto jsonResult = JsonParser::parseApiJsonString(body, "creature registration request", parseSpan);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), requestSpan);
    }
    const auto registrationResult = api::registerCreatureRequestFromJson(jsonResult.getValue().value());
    if (!registrationResult.isSuccess()) {
        const auto error = registrationResult.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidRegistrationEnvelope", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }
    const auto registration = registrationResult.getValue().value();
    if (requestSpan) {
        requestSpan->setAttribute("universe", static_cast<int64_t>(registration.universe));
        requestSpan->setAttribute("creature.config.size", static_cast<int64_t>(registration.creatureConfig.size()));
    }
    const auto result = creatures::ws::CreatureService::registerCreature(registration.creatureConfig,
                                                                         registration.universe, nullptr, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    const auto response = result.getValue().value();
    if (requestSpan) {
        requestSpan->setAttribute("creature.id", response.creature.id);
        requestSpan->setAttribute("creature.name", response.creature.name);
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::creatureResponseToJson(response)));
}

PreparedResponse exportCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("creature.id", creatureId);
    }
    if (!isUuidShape(creatureId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "creatureId must be a UUID"), requestSpan);
    }
    if (!creatures::db) {
        return errorResponse(ServerError(ServerError::InternalError, "Creature database unavailable"), requestSpan);
    }
    const auto result = creatures::db->getCreatureJson(creatureId, requestSpan);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    auto creatureJson = result.getValue().value();
    creatureJson.erase("_id");
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(creatureJson));
}

} // namespace creatures::transport
