#include "server/transport/FixtureWriteHandlers.h"

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "api/FixtureRequests.h"
#include "api/JsonResponse.h"
#include "server/ws/service/DmxFixtureService.h"
#include "util/JsonParser.h"
#include "util/UuidValidation.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

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

std::shared_ptr<OperationSpan> createParseSpan(const std::string &name,
                                               const std::shared_ptr<OperationSpan> &parentSpan) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, parentSpan) : nullptr;
}

PreparedResponse fixtureResponse(const Result<DmxFixture> &result, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", result.getValue()->id);
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(dmxFixtureToJson(result.getValue().value())));
}

PreparedResponse invalidFixtureId(const std::shared_ptr<OperationSpan> &requestSpan) {
    return errorResponse(ServerError(ServerError::InvalidData, "fixtureId must be a UUID"), requestSpan);
}

} // namespace

PreparedResponse upsertFixture(const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("request.body_size", static_cast<int64_t>(body.size()));
    }
    return fixtureResponse(creatures::ws::DmxFixtureService::upsertFixture(body), requestSpan);
}

PreparedResponse validateFixture(const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    const auto result = creatures::ws::DmxFixtureService::validateFixtureConfig(body);
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(200, api::jsonToString(api::fixtureConfigValidationResponseToJson(result)));
}

PreparedResponse deleteFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    const auto result = creatures::ws::DmxFixtureService::deleteFixture(fixtureId);
    if (!result.isSuccess()) {
        return errorResponse(result.getError().value(), requestSpan);
    }
    if (requestSpan) {
        requestSpan->setSuccess();
    }
    return PreparedResponse::json(
        200, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(200, "Fixture deleted"))));
}

PreparedResponse setFixtureUniverse(const fixtureId_t &fixtureId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    auto parseSpan = createParseSpan("DmxFixtureController.parseSetUniverseRequest", requestSpan);
    const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture universe request", parseSpan);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), requestSpan);
    }
    const auto parsed = api::setFixtureUniverseRequestFromJson(jsonResult.getValue().value());
    if (!parsed.isSuccess()) {
        const auto error = parsed.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidFixtureUniverseRequest", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }
    if (requestSpan) {
        requestSpan->setAttribute("fixture.universe", static_cast<int64_t>(parsed.getValue()->universe));
    }
    return fixtureResponse(creatures::ws::DmxFixtureService::setFixtureUniverse(fixtureId, parsed.getValue()->universe),
                           requestSpan);
}

PreparedResponse clearFixtureUniverse(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    return fixtureResponse(creatures::ws::DmxFixtureService::setFixtureUniverse(fixtureId, std::nullopt), requestSpan);
}

PreparedResponse triggerFixturePattern(const fixtureId_t &fixtureId, const std::string &patternId,
                                       const std::string &body, const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
        requestSpan->setAttribute("pattern.id", patternId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    if (!isUuidShape(patternId)) {
        return errorResponse(ServerError(ServerError::InvalidData, "patternId must be a UUID"), requestSpan);
    }
    nlohmann::json rawBody = nlohmann::json::object();
    auto parseSpan = createParseSpan("DmxFixtureController.parseTriggerPatternRequest", requestSpan);
    if (!body.empty()) {
        const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture pattern trigger request", parseSpan);
        if (!jsonResult.isSuccess()) {
            return errorResponse(jsonResult.getError().value(), requestSpan);
        }
        rawBody = jsonResult.getValue().value();
    }
    const auto parsed = api::triggerFixturePatternRequestFromJson(rawBody);
    if (!parsed.isSuccess()) {
        const auto error = parsed.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidFixturePatternTriggerRequest", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }
    return fixtureResponse(
        creatures::ws::DmxFixtureService::triggerPattern(fixtureId, patternId, parsed.getValue()->stopAfterMs),
        requestSpan);
}

PreparedResponse previewFixturePattern(const fixtureId_t &fixtureId, const std::string &body,
                                       const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    auto parseSpan = createParseSpan("DmxFixtureController.parsePreviewPatternRequest", requestSpan);
    const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture pattern preview request", parseSpan);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), requestSpan);
    }
    const auto parsedResult = api::previewFixturePatternRequestFromJson(jsonResult.getValue().value());
    if (!parsedResult.isSuccess()) {
        const auto error = parsedResult.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidFixturePatternPreviewRequest", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }
    const auto parsed = parsedResult.getValue().value();
    std::vector<std::pair<std::string, uint8_t>> values;
    values.reserve(parsed.values.size());
    for (const auto &value : parsed.values) {
        values.emplace_back(value.channel, value.value);
    }
    return fixtureResponse(creatures::ws::DmxFixtureService::previewPattern(
                               fixtureId, values, parsed.fadeInMs, parsed.fadeOutMs, parsed.holdMs, parsed.stopAfterMs),
                           requestSpan);
}

PreparedResponse setFixtureLive(const fixtureId_t &fixtureId, const std::string &body,
                                const std::shared_ptr<OperationSpan> &requestSpan) {
    if (requestSpan) {
        requestSpan->setAttribute("fixture.id", fixtureId);
    }
    if (!isUuidShape(fixtureId)) {
        return invalidFixtureId(requestSpan);
    }
    auto parseSpan = createParseSpan("DmxFixtureController.parseLiveRequest", requestSpan);
    const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture live request", parseSpan);
    if (!jsonResult.isSuccess()) {
        return errorResponse(jsonResult.getError().value(), requestSpan);
    }
    const auto parsedResult = api::setFixtureLiveRequestFromJson(jsonResult.getValue().value());
    if (!parsedResult.isSuccess()) {
        const auto error = parsedResult.getError().value();
        recordSpanError(parseSpan, error.getMessage(), "InvalidFixtureLiveRequest", error.getCode());
        return errorResponse(error, requestSpan);
    }
    if (parseSpan) {
        parseSpan->setSuccess();
    }
    const auto parsed = parsedResult.getValue().value();
    std::vector<std::pair<std::string, uint8_t>> values;
    values.reserve(parsed.values.size());
    for (const auto &value : parsed.values) {
        values.emplace_back(value.channel, value.value);
    }
    return fixtureResponse(creatures::ws::DmxFixtureService::setFixtureLive(fixtureId, values, parsed.timeoutMs),
                           requestSpan);
}

} // namespace creatures::transport
