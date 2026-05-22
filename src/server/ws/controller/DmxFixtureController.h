#pragma once

#include <optional>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "oatpp/core/Types.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <nlohmann/json.hpp>

#include "server/config.h"
#include "server/database.h"
#include "server/metrics/counters.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/dto/FixtureConfigValidationDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/SetFixtureUniverseRequestDto.h"
#include "server/ws/dto/TriggerFixturePatternRequestDto.h"
#include "server/ws/service/DmxFixtureService.h"
#include "util/websocketUtils.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class DmxFixtureController : public oatpp::web::server::api::ApiController {
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
        info->addResponse<Object<ListDto<Object<creatures::DmxFixtureDto>>>>(Status::CODE_200,
                                                                             "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/fixture", getAllFixtures,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span = creatures::observability
                              ? creatures::observability->createRequestSpan(
                                    "GET /api/v1/fixture", "GET", "api/v1/fixture", extractTraceparent(request))
                              : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "getAllFixtures");
            span->setAttribute("controller", "DmxFixtureController");
        }

        const auto result = m_service.getAllFixtures(span);

        if (span)
            span->setHttpStatus(200);
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(getFixture) {
        info->summary = "Get one DMX fixture by id";
        info->addTag("Fixtures");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("GET", "api/v1/fixture/{fixtureId}", getFixture, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span = creatures::observability
                              ? creatures::observability->createRequestSpan("GET /api/v1/fixture/{fixtureId}", "GET",
                                                                            "api/v1/fixture/" + std::string(fixtureId),
                                                                            extractTraceparent(request))
                              : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "getFixture");
            span->setAttribute("controller", "DmxFixtureController");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        const auto result = m_service.getFixture(fixtureId, span);

        if (span)
            span->setHttpStatus(200);
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(upsertFixture) {
        info->summary = "Create or update a DMX fixture";
        info->description = "Accepts raw fixture JSON and upserts it to the database. Required fields: id, name, "
                            "type, channel_offset, channels.";
        info->addTag("Fixtures");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/fixture", upsertFixture, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span = creatures::observability
                              ? creatures::observability->createRequestSpan(
                                    "POST /api/v1/fixture", "POST", "api/v1/fixture", extractTraceparent(request))
                              : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();

        try {
            const auto fixtureConfig = std::string(body);
            if (span) {
                span->setAttribute("endpoint", "upsertFixture");
                span->setAttribute("controller", "DmxFixtureController");
                span->setAttribute("request.body_size", static_cast<int64_t>(fixtureConfig.length()));
            }

            const auto result = m_service.upsertFixture(fixtureConfig, span);

            if (span) {
                span->setAttribute("fixture.id", std::string(result->id));
                span->setHttpStatus(200);
            }

            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
            return createDtoResponse(Status::CODE_200, result);
        } catch (const std::exception &ex) {
            if (span) {
                span->recordException(ex);
                span->setHttpStatus(500);
            }
            error("Exception in upsertFixture: {}", ex.what());
            throw;
        }
    }

    ENDPOINT_INFO(deleteFixture) {
        info->summary = "Delete a DMX fixture";
        info->addTag("Fixtures");
        info->addResponse<Object<StatusDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("DELETE", "api/v1/fixture/{fixtureId}", deleteFixture, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span = creatures::observability
                              ? creatures::observability->createRequestSpan(
                                    "DELETE /api/v1/fixture/{fixtureId}", "DELETE",
                                    "api/v1/fixture/" + std::string(fixtureId), extractTraceparent(request))
                              : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "deleteFixture");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        m_service.deleteFixture(fixtureId, span);

        if (span)
            span->setHttpStatus(200);
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);

        const auto ok = StatusDto::createShared();
        ok->status = "OK";
        ok->code = 200;
        ok->message = "Fixture deleted";
        return createDtoResponse(Status::CODE_200, ok);
    }

    ENDPOINT_INFO(validateFixtureConfig) {
        info->summary = "Validate a fixture config payload without saving";
        info->addTag("Fixtures");
        info->addResponse<Object<FixtureConfigValidationDto>>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/fixture/validate", validateFixtureConfig, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span =
            creatures::observability
                ? creatures::observability->createRequestSpan("POST /api/v1/fixture/validate", "POST",
                                                              "api/v1/fixture/validate", extractTraceparent(request))
                : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "validateFixtureConfig");
            span->setAttribute("controller", "DmxFixtureController");
            span->setAttribute("request.body_size", static_cast<int64_t>(body ? body->size() : 0));
        }

        const auto result = m_service.validateFixtureConfig(std::string(body), span);
        if (span)
            span->setHttpStatus(200);
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(setFixtureUniverse) {
        info->summary = "Assign a fixture's DMX universe (persisted)";
        info->description = "Sets the fixture's `assigned_universe` field in MongoDB and mirrors it to the runtime "
                            "lookup map. Survives server restart.";
        info->addTag("Fixtures");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("PUT", "api/v1/fixture/{fixtureId}/universe", setFixtureUniverse, PATH(String, fixtureId),
             BODY_DTO(Object<SetFixtureUniverseRequestDto>, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span =
            creatures::observability
                ? creatures::observability->createRequestSpan("PUT /api/v1/fixture/{fixtureId}/universe", "PUT",
                                                              "api/v1/fixture/" + std::string(fixtureId) + "/universe",
                                                              extractTraceparent(request))
                : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();

        if (!body || body->universe == nullptr) {
            OATPP_ASSERT_HTTP(false, Status::CODE_400, "Request body must include 'universe'");
        }
        const universe_t universe = static_cast<universe_t>(*body->universe);

        if (span) {
            span->setAttribute("endpoint", "setFixtureUniverse");
            span->setAttribute("fixture.id", std::string(fixtureId));
            span->setAttribute("fixture.universe", static_cast<int64_t>(universe));
        }

        const auto result = m_service.setFixtureUniverse(fixtureId, std::optional<universe_t>{universe}, span);

        if (span)
            span->setHttpStatus(200);
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(triggerFixturePattern) {
        info->summary = "Manually trigger a fixture pattern (bypasses binding match)";
        info->description = "Fires the pattern directly. Useful for ad-hoc UI control and testing. The fixture must "
                            "have an assigned universe (`PUT /api/v1/fixture/{id}/universe`).";
        info->addTag("Fixtures");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
        info->pathParams["patternId"].description = "Pattern UUID (must exist on the fixture)";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger", triggerFixturePattern,
             PATH(String, fixtureId), PATH(String, patternId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span =
            creatures::observability
                ? creatures::observability->createRequestSpan(
                      "POST /api/v1/fixture/{fixtureId}/pattern/{patternId}/trigger", "POST",
                      "api/v1/fixture/" + std::string(fixtureId) + "/pattern/" + std::string(patternId) + "/trigger",
                      extractTraceparent(request))
                : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "triggerFixturePattern");
            span->setAttribute("fixture.id", std::string(fixtureId));
            span->setAttribute("pattern.id", std::string(patternId));
        }

        // Body is optional. If present, it must parse cleanly.
        std::optional<uint32_t> stopAfterMs;
        const oatpp::String body = request->readBodyToString();
        if (body && body->size() > 0) {
            try {
                const auto parsed = nlohmann::json::parse(std::string(body));
                if (parsed.contains("stop_after_ms") && !parsed["stop_after_ms"].is_null()) {
                    const auto raw = parsed["stop_after_ms"].get<uint32_t>();
                    // Cap at 10 minutes. UInt32 max would schedule ~50 days of stuck DMX
                    // and a far-future AutoStopEvent pinned in the event-loop priority
                    // queue. Reject 0 explicitly — "stop immediately" is a footgun.
                    constexpr uint32_t MAX_STOP_AFTER_MS = 10 * 60 * 1000;
                    if (raw == 0) {
                        OATPP_ASSERT_HTTP(false, Status::CODE_400,
                                          "stop_after_ms must be > 0 (omit it to disable auto-stop)");
                    }
                    if (raw > MAX_STOP_AFTER_MS) {
                        OATPP_ASSERT_HTTP(false, Status::CODE_400,
                                          fmt::format("stop_after_ms must be <= {} ms (10 min); got {}",
                                                      MAX_STOP_AFTER_MS, raw)
                                              .c_str());
                    }
                    stopAfterMs = raw;
                }
            } catch (const std::exception &e) {
                OATPP_ASSERT_HTTP(false, Status::CODE_400, fmt::format("Invalid trigger body: {}", e.what()).c_str());
            }
        }

        const auto result = m_service.triggerPattern(fixtureId, patternId, stopAfterMs, span);

        if (span)
            span->setHttpStatus(200);
        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(clearFixtureUniverse) {
        info->summary = "Clear a fixture's DMX universe assignment";
        info->addTag("Fixtures");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("DELETE", "api/v1/fixture/{fixtureId}/universe", clearFixtureUniverse, PATH(String, fixtureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span =
            creatures::observability
                ? creatures::observability->createRequestSpan("DELETE /api/v1/fixture/{fixtureId}/universe", "DELETE",
                                                              "api/v1/fixture/" + std::string(fixtureId) + "/universe",
                                                              extractTraceparent(request))
                : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint", "clearFixtureUniverse");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        const auto result = m_service.setFixtureUniverse(fixtureId, std::nullopt, span);

        if (span)
            span->setHttpStatus(200);
        scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
        return createDtoResponse(Status::CODE_200, result);
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
