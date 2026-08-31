#pragma once

#include <optional>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "oatpp/core/Types.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <nlohmann/json.hpp>

#include "api/FixtureRequests.h"
#include "server/config.h"
#include "server/database.h"
#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/service/DmxFixtureService.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/websocketUtils.h"

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class DmxFixtureController : public oatpp::web::server::api::ApiController,
                             public HttpResponseHelpers<DmxFixtureController> {
  public:
    DmxFixtureController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

  private:
    DmxFixtureService m_service;

  public:
    static std::shared_ptr<DmxFixtureController> createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                                                              objectMapper)) {
        return std::make_shared<DmxFixtureController>(objectMapper);
    }

    ENDPOINT_INFO(getAllFixtures) {
        info->summary = "List all DMX fixtures";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/fixture", getAllFixtures,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint("GET /api/v1/fixture", "GET", "api/v1/fixture", "getAllFixtures", "DmxFixtureController",
                           request, [&](const auto &span) {
                               const auto result = m_service.getAllFixtures(span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(
                                   span, Status::CODE_200,
                                   api::listResponseToJson(result.getValue().value(), dmxFixtureToJson));
                           });
    }

    ENDPOINT_INFO(getFixture) {
        info->summary = "Get one DMX fixture by id";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("GET", "api/v1/fixture/{fixtureId}", getFixture, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint("GET /api/v1/fixture/{fixtureId}", "GET", "api/v1/fixture/" + std::string(fixtureId),
                           "getFixture", "DmxFixtureController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("fixture.id", std::string(fixtureId));
                               if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                                   return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                               }
                               const auto result = m_service.getFixture(std::string(fixtureId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(upsertFixture) {
        info->summary = "Create or update a DMX fixture";
        info->description = "Accepts raw fixture JSON and upserts it to the database. Required fields: id, name, "
                            "type, channel_offset, channels.";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/fixture", upsertFixture,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint("POST /api/v1/fixture", "POST", "api/v1/fixture", "upsertFixture", "DmxFixtureController",
                           request, [&](const auto &span) {
                               const auto fixtureConfig =
                                   readRequestBodyLimited(request, api::MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES, span);
                               if (span) {
                                   span->setAttribute("request.body_size",
                                                      static_cast<int64_t>(fixtureConfig.length()));
                               }
                               const auto result = m_service.upsertFixture(fixtureConfig, span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span) {
                                   span->setAttribute("fixture.id", result.getValue()->id);
                                   span->setHttpStatus(200);
                               }
                               return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
                           });
    }

    ENDPOINT_INFO(deleteFixture) {
        info->summary = "Delete a DMX fixture";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("DELETE", "api/v1/fixture/{fixtureId}", deleteFixture, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint("DELETE /api/v1/fixture/{fixtureId}", "DELETE", "api/v1/fixture/" + std::string(fixtureId),
                           "deleteFixture", "DmxFixtureController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("fixture.id", std::string(fixtureId));
                               if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                                   return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                               }
                               const auto result = m_service.deleteFixture(std::string(fixtureId), span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return okStatus(span, Status::CODE_200, "Fixture deleted");
                           });
    }

    ENDPOINT_INFO(validateFixtureConfig) {
        info->summary = "Validate a fixture config payload without saving";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/fixture/validate", validateFixtureConfig,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint(
            "POST /api/v1/fixture/validate", "POST", "api/v1/fixture/validate", "validateFixtureConfig",
            "DmxFixtureController", request, [&](const auto &span) {
                const auto body = readRequestBodyLimited(request, api::MAX_FIXTURE_CONFIG_REQUEST_BODY_BYTES, span);
                const auto result = m_service.validateFixtureConfig(body, span);
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, api::fixtureConfigValidationResponseToJson(result));
            });
    }

    ENDPOINT_INFO(setFixtureUniverse) {
        info->summary = "Assign a fixture's DMX universe (persisted)";
        info->description = "Sets the fixture's `assigned_universe` field in MongoDB and mirrors it to the runtime "
                            "lookup map. Survives server restart.";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("PUT", "api/v1/fixture/{fixtureId}/universe", setFixtureUniverse, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint(
            "PUT /api/v1/fixture/{fixtureId}/universe", "PUT", "api/v1/fixture/" + std::string(fixtureId) + "/universe",
            "setFixtureUniverse", "DmxFixtureController", request, [&](const auto &span) {
                if (span)
                    span->setAttribute("fixture.id", std::string(fixtureId));
                if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                    return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan =
                    observability
                        ? observability->createChildOperationSpan("DmxFixtureController.parseSetUniverseRequest", span)
                        : nullptr;
                const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture universe request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto requestResult = api::setFixtureUniverseRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidFixtureUniverseRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto universe = requestResult.getValue()->universe;
                if (span)
                    span->setAttribute("fixture.universe", static_cast<int64_t>(universe));
                const auto result = m_service.setFixtureUniverse(std::string(fixtureId), universe, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(triggerFixturePattern) {
        info->summary = "Manually trigger a fixture pattern (bypasses binding match)";
        info->description = "Fires the pattern directly. Useful for ad-hoc UI control and testing. The fixture must "
                            "have an assigned universe (`PUT /api/v1/fixture/{id}/universe`).";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
        info->pathParams["patternId"].description = "Pattern UUID (must exist on the fixture)";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger", triggerFixturePattern,
             PATH(String, fixtureId), PATH(String, patternId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint(
            "POST /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger", "POST",
            "api/v1/fixture/" + std::string(fixtureId) + "/pattern/" + std::string(patternId) + "/trigger",
            "triggerFixturePattern", "DmxFixtureController", request, [&](const auto &span) {
                if (span) {
                    span->setAttribute("fixture.id", std::string(fixtureId));
                    span->setAttribute("pattern.id", std::string(patternId));
                }
                if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                    return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                }
                if (!patternId || !isUuidShape(std::string(patternId))) {
                    return bailHttp(span, Status::CODE_400, "patternId must be a UUID");
                }

                const auto body = readRequestBodyLimited(request, api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES, span);
                nlohmann::json rawBody = nlohmann::json::object();
                const auto parseSpan = observability ? observability->createChildOperationSpan(
                                                           "DmxFixtureController.parseTriggerPatternRequest", span)
                                                     : nullptr;
                if (!body.empty()) {
                    const auto jsonResult =
                        JsonParser::parseApiJsonString(body, "fixture pattern trigger request", parseSpan);
                    if (!jsonResult.isSuccess())
                        return bailFromServerError(span, jsonResult.getError().value());
                    rawBody = jsonResult.getValue().value();
                }
                const auto requestResult = api::triggerFixturePatternRequestFromJson(rawBody);
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidFixturePatternTriggerRequest",
                                    error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto result = m_service.triggerPattern(std::string(fixtureId), std::string(patternId),
                                                             requestResult.getValue()->stopAfterMs, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(previewFixturePattern) {
        info->summary = "Fire an ephemeral, not-persisted pattern (editor preview)";
        info->description =
            "Same runner path as `triggerFixturePattern`, but the pattern is built from the request body instead of "
            "looked up by id. Intended for the Creature Console pattern editor's Fire button so unsaved edits can be "
            "previewed without an upsert round-trip. The fixture must have an assigned universe. Live control "
            "preempts: if a live session is active for this fixture, the preview is refused with a 400.";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/pattern/preview", previewFixturePattern, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint(
            "POST /api/v1/fixture/{fixtureId}/pattern/preview", "POST",
            "api/v1/fixture/" + std::string(fixtureId) + "/pattern/preview", "previewFixturePattern",
            "DmxFixtureController", request, [&](const auto &span) {
                if (span)
                    span->setAttribute("fixture.id", std::string(fixtureId));
                if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                    return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan = observability ? observability->createChildOperationSpan(
                                                           "DmxFixtureController.parsePreviewPatternRequest", span)
                                                     : nullptr;
                const auto jsonResult =
                    JsonParser::parseApiJsonString(body, "fixture pattern preview request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto requestResult = api::previewFixturePatternRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidFixturePatternPreviewRequest",
                                    error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto parsed = requestResult.getValue().value();
                std::vector<std::pair<std::string, uint8_t>> channelValues;
                channelValues.reserve(parsed.values.size());
                for (const auto &value : parsed.values)
                    channelValues.emplace_back(value.channel, value.value);

                if (span) {
                    span->setAttribute("pattern.preview.value_count", static_cast<int64_t>(channelValues.size()));
                    span->setAttribute("pattern.fade_in_ms", static_cast<int64_t>(parsed.fadeInMs));
                    span->setAttribute("pattern.fade_out_ms", static_cast<int64_t>(parsed.fadeOutMs));
                    span->setAttribute("pattern.hold_ms", static_cast<int64_t>(parsed.holdMs));
                }

                const auto result = m_service.previewPattern(std::string(fixtureId), channelValues, parsed.fadeInMs,
                                                             parsed.fadeOutMs, parsed.holdMs, parsed.stopAfterMs, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(setFixtureLive) {
        info->summary = "Drive a fixture's channels directly with raw DMX values (slider UI)";
        info->description =
            "Bypasses patterns and bindings to write per-channel values straight to DMX. Useful for slider-driven "
            "tuning in the Creature Console. The active pattern (if any) is cancelled immediately on first live call. "
            "The server holds the values until `timeout_ms` elapses, then blacks out the fixture's channels. New "
            "patterns cannot start on this fixture until the live session expires. Channels not named in `values` "
            "hold their previous live value (or default to 0 on the first call).";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/live", setFixtureLive, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint(
            "POST /api/v1/fixture/{fixtureId}/live", "POST", "api/v1/fixture/" + std::string(fixtureId) + "/live",
            "setFixtureLive", "DmxFixtureController", request, [&](const auto &span) {
                if (span)
                    span->setAttribute("fixture.id", std::string(fixtureId));
                if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                    return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                }
                const auto body = readRequestBodyLimited(request, api::MAX_FIXTURE_CONTROL_REQUEST_BODY_BYTES, span);
                const auto parseSpan =
                    observability
                        ? observability->createChildOperationSpan("DmxFixtureController.parseLiveRequest", span)
                        : nullptr;
                const auto jsonResult = JsonParser::parseApiJsonString(body, "fixture live request", parseSpan);
                if (!jsonResult.isSuccess())
                    return bailFromServerError(span, jsonResult.getError().value());
                const auto requestResult = api::setFixtureLiveRequestFromJson(jsonResult.getValue().value());
                if (!requestResult.isSuccess()) {
                    const auto error = requestResult.getError().value();
                    recordSpanError(parseSpan, error.getMessage(), "InvalidFixtureLiveRequest", error.getCode());
                    return bailFromServerError(span, error);
                }
                if (parseSpan)
                    parseSpan->setSuccess();
                const auto parsed = requestResult.getValue().value();
                std::vector<std::pair<std::string, uint8_t>> channelValues;
                channelValues.reserve(parsed.values.size());
                for (const auto &value : parsed.values)
                    channelValues.emplace_back(value.channel, value.value);

                if (span)
                    span->setAttribute("fixture.live.value_count", static_cast<int64_t>(channelValues.size()));

                const auto result =
                    m_service.setFixtureLive(std::string(fixtureId), channelValues, parsed.timeoutMs, span);
                if (!result.isSuccess())
                    return bailFromServerError(span, result.getError().value());
                if (span)
                    span->setHttpStatus(200);
                return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
            });
    }

    ENDPOINT_INFO(clearFixtureUniverse) {
        info->summary = "Clear a fixture's DMX universe assignment";
        info->addTag("Fixtures");
        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<oatpp::String>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("DELETE", "api/v1/fixture/{fixtureId}/universe", clearFixtureUniverse, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        return runEndpoint("DELETE /api/v1/fixture/{fixtureId}/universe", "DELETE",
                           "api/v1/fixture/" + std::string(fixtureId) + "/universe", "clearFixtureUniverse",
                           "DmxFixtureController", request, [&](const auto &span) {
                               if (span)
                                   span->setAttribute("fixture.id", std::string(fixtureId));
                               if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                                   return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
                               }
                               const auto result =
                                   m_service.setFixtureUniverse(std::string(fixtureId), std::nullopt, span);
                               if (!result.isSuccess())
                                   return bailFromServerError(span, result.getError().value());
                               if (span)
                                   span->setHttpStatus(200);
                               return jsonResponse(span, Status::CODE_200, dmxFixtureToJson(result.getValue().value()));
                           });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
