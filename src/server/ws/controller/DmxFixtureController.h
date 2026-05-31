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
#include "server/ws/controller/HttpResponseHelpers.h"
#include "server/ws/dto/FixtureConfigValidationDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/PreviewFixturePatternRequestDto.h"
#include "server/ws/dto/SetFixtureLiveRequestDto.h"
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
            span->setAttribute("endpoint.name", "getAllFixtures");
            span->setAttribute("controller.name", "DmxFixtureController");
        }

        return withSpanStatus(span, [&] {
            const auto result = m_service.getAllFixtures(span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
        });
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
            span->setAttribute("endpoint.name", "getFixture");
            span->setAttribute("controller.name", "DmxFixtureController");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            const auto result = m_service.getFixture(fixtureId, span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
        });
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

        return withSpanStatus(span, [&] {
            const auto fixtureConfig = std::string(body);
            if (span) {
                span->setAttribute("endpoint.name", "upsertFixture");
                span->setAttribute("controller.name", "DmxFixtureController");
                span->setAttribute("request.body_size", static_cast<int64_t>(fixtureConfig.length()));
            }
            const auto result = m_service.upsertFixture(fixtureConfig, span);
            if (span) {
                span->setAttribute("fixture.id", std::string(result->id));
                span->setHttpStatus(200);
            }
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
            return createDtoResponse(Status::CODE_200, result);
        });
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
            span->setAttribute("endpoint.name", "deleteFixture");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            m_service.deleteFixture(fixtureId, span);
            if (span)
                span->setHttpStatus(200);
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
            return okStatus(span, Status::CODE_200, "Fixture deleted");
        });
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
            span->setAttribute("endpoint.name", "validateFixtureConfig");
            span->setAttribute("controller.name", "DmxFixtureController");
            span->setAttribute("request.body_size", static_cast<int64_t>(body ? body->size() : 0));
        }

        return withSpanStatus(span, [&] {
            const auto result = m_service.validateFixtureConfig(std::string(body), span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
        });
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

        if (span) {
            span->setAttribute("endpoint.name", "setFixtureUniverse");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            if (!body || body->universe == nullptr) {
                return bailHttp(span, Status::CODE_400, "Request body must include 'universe'");
            }
            const universe_t universe = static_cast<universe_t>(*body->universe);
            // E1.31 universes are valid in [1, 63999]. Reject 0 (reserved) and >63999.
            if (universe < 1 || universe > 63999) {
                return bailHttp(span, Status::CODE_400, "universe must be in [1, 63999]");
            }
            if (span)
                span->setAttribute("fixture.universe", static_cast<int64_t>(universe));
            const auto result = m_service.setFixtureUniverse(fixtureId, std::optional<universe_t>{universe}, span);
            if (span)
                span->setHttpStatus(200);
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
            return createDtoResponse(Status::CODE_200, result);
        });
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
            span->setAttribute("endpoint.name", "triggerFixturePattern");
            span->setAttribute("fixture.id", std::string(fixtureId));
            span->setAttribute("pattern.id", std::string(patternId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            if (!patternId || !isUuidShape(std::string(patternId))) {
                return bailHttp(span, Status::CODE_400, "patternId must be a UUID");
            }

            // Body is optional. If present, it must parse cleanly.
            std::optional<uint32_t> stopAfterMs;
            const oatpp::String body = request->readBodyToString();
            if (body && body->size() > 0) {
                nlohmann::json parsed;
                try {
                    parsed = nlohmann::json::parse(std::string(body));
                } catch (const nlohmann::json::exception &e) {
                    return bailHttp(span, Status::CODE_400, fmt::format("Invalid trigger body: {}", e.what()));
                }
                if (parsed.contains("stop_after_ms") && !parsed["stop_after_ms"].is_null()) {
                    const auto raw = parsed["stop_after_ms"].get<uint32_t>();
                    // Cap at 10 minutes. UInt32 max would schedule ~50 days of stuck DMX
                    // and a far-future AutoStopEvent pinned in the event-loop priority
                    // queue. Reject 0 explicitly — "stop immediately" is a footgun.
                    constexpr uint32_t MAX_STOP_AFTER_MS = 10 * 60 * 1000;
                    if (raw == 0) {
                        return bailHttp(span, Status::CODE_400,
                                        "stop_after_ms must be > 0 (omit it to disable auto-stop)");
                    }
                    if (raw > MAX_STOP_AFTER_MS) {
                        return bailHttp(
                            span, Status::CODE_400,
                            fmt::format("stop_after_ms must be <= {} ms (10 min); got {}", MAX_STOP_AFTER_MS, raw));
                    }
                    stopAfterMs = raw;
                }
            }

            const auto result = m_service.triggerPattern(fixtureId, patternId, stopAfterMs, span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
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
        info->addConsumes<Object<PreviewFixturePatternRequestDto>>("application/json");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/pattern/preview", previewFixturePattern, PATH(String, fixtureId),
             BODY_DTO(Object<PreviewFixturePatternRequestDto>, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span =
            creatures::observability
                ? creatures::observability->createRequestSpan(
                      "POST /api/v1/fixture/{fixtureId}/pattern/preview", "POST",
                      "api/v1/fixture/" + std::string(fixtureId) + "/pattern/preview", extractTraceparent(request))
                : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint.name", "previewFixturePattern");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            if (!body) {
                return bailHttp(span, Status::CODE_400, "Request body is required");
            }
            if (!body->values) {
                return bailHttp(span, Status::CODE_400, "values array is required");
            }
            if (body->values->size() == 0) {
                return bailHttp(span, Status::CODE_400, "values must contain at least one channel");
            }

            // Unpack DTO to a flat vector for the service.
            std::vector<std::pair<std::string, uint8_t>> channelValues;
            channelValues.reserve(body->values->size());
            for (const auto &v : *body->values) {
                if (!v) {
                    return bailHttp(span, Status::CODE_400, "values entries must be objects, not null");
                }
                if (!v->channel) {
                    return bailHttp(span, Status::CODE_400, "values[].channel is required");
                }
                if (!v->value) {
                    return bailHttp(span, Status::CODE_400, "values[].value is required");
                }
                channelValues.emplace_back(std::string(v->channel), static_cast<uint8_t>(*v->value));
            }

            // Optional timing fields default to 0 (snap / hold-forever). stop_after_ms gets the
            // same cap as triggerFixturePattern — UInt32 max would pin a far-future event on the
            // event loop's priority queue.
            const uint32_t fadeInMs = body->fade_in_ms ? static_cast<uint32_t>(*body->fade_in_ms) : 0;
            const uint32_t fadeOutMs = body->fade_out_ms ? static_cast<uint32_t>(*body->fade_out_ms) : 0;
            const uint32_t holdMs = body->hold_ms ? static_cast<uint32_t>(*body->hold_ms) : 0;
            std::optional<uint32_t> stopAfterMs;
            if (body->stop_after_ms) {
                const uint32_t raw = static_cast<uint32_t>(*body->stop_after_ms);
                constexpr uint32_t MAX_STOP_AFTER_MS = 10 * 60 * 1000;
                if (raw == 0) {
                    return bailHttp(span, Status::CODE_400, "stop_after_ms must be > 0 (omit it to disable auto-stop)");
                }
                if (raw > MAX_STOP_AFTER_MS) {
                    return bailHttp(
                        span, Status::CODE_400,
                        fmt::format("stop_after_ms must be <= {} ms (10 min); got {}", MAX_STOP_AFTER_MS, raw));
                }
                stopAfterMs = raw;
            }

            if (span) {
                span->setAttribute("pattern.preview.value_count", static_cast<int64_t>(channelValues.size()));
                span->setAttribute("pattern.fade_in_ms", static_cast<int64_t>(fadeInMs));
                span->setAttribute("pattern.fade_out_ms", static_cast<int64_t>(fadeOutMs));
                span->setAttribute("pattern.hold_ms", static_cast<int64_t>(holdMs));
            }

            const auto result =
                m_service.previewPattern(fixtureId, channelValues, fadeInMs, fadeOutMs, holdMs, stopAfterMs, span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
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
        info->addConsumes<Object<SetFixtureLiveRequestDto>>("application/json");
        info->addResponse<Object<creatures::DmxFixtureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->pathParams["fixtureId"].description = "Fixture UUID";
    }
    ENDPOINT("POST", "api/v1/fixture/{fixtureId}/live", setFixtureLive, PATH(String, fixtureId),
             BODY_DTO(Object<SetFixtureLiveRequestDto>, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        const auto span = creatures::observability
                              ? creatures::observability->createRequestSpan(
                                    "POST /api/v1/fixture/{fixtureId}/live", "POST",
                                    "api/v1/fixture/" + std::string(fixtureId) + "/live", extractTraceparent(request))
                              : nullptr;
        addHttpRequestAttributes(span, request);
        if (creatures::metrics)
            creatures::metrics->incrementRestRequestsProcessed();
        if (span) {
            span->setAttribute("endpoint.name", "setFixtureLive");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            if (!body) {
                return bailHttp(span, Status::CODE_400, "Request body is required");
            }
            if (!body->values) {
                return bailHttp(span, Status::CODE_400, "values array is required");
            }
            if (body->values->size() == 0) {
                return bailHttp(span, Status::CODE_400, "values must contain at least one channel");
            }
            if (!body->timeout_ms) {
                return bailHttp(span, Status::CODE_400, "timeout_ms is required");
            }

            // Unpack DTO to a flat vector for the service. We surface a clean 400 here for
            // malformed entries (missing channel name, missing value) so the service doesn't
            // have to handle DTO-level shape errors.
            std::vector<std::pair<std::string, uint8_t>> channelValues;
            channelValues.reserve(body->values->size());
            for (const auto &v : *body->values) {
                if (!v) {
                    return bailHttp(span, Status::CODE_400, "values entries must be objects, not null");
                }
                if (!v->channel) {
                    return bailHttp(span, Status::CODE_400, "values[].channel is required");
                }
                if (!v->value) {
                    return bailHttp(span, Status::CODE_400, "values[].value is required");
                }
                channelValues.emplace_back(std::string(v->channel), static_cast<uint8_t>(*v->value));
            }
            const uint32_t timeoutMs = *body->timeout_ms;

            if (span)
                span->setAttribute("fixture.live.value_count", static_cast<int64_t>(channelValues.size()));

            const auto result = m_service.setFixtureLive(fixtureId, channelValues, timeoutMs, span);
            if (span)
                span->setHttpStatus(200);
            return createDtoResponse(Status::CODE_200, result);
        });
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
            span->setAttribute("endpoint.name", "clearFixtureUniverse");
            span->setAttribute("fixture.id", std::string(fixtureId));
        }

        return withSpanStatus(span, [&] {
            if (!fixtureId || !isUuidShape(std::string(fixtureId))) {
                return bailHttp(span, Status::CODE_400, "fixtureId must be a UUID");
            }
            const auto result = m_service.setFixtureUniverse(fixtureId, std::nullopt, span);
            if (span)
                span->setHttpStatus(200);
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Fixture);
            return createDtoResponse(Status::CODE_200, result);
        });
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)
