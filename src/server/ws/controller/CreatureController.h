#pragma once

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/component.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/web/protocol/http/Http.hpp"
#include "oatpp/web/server/AsyncHttpConnectionHandler.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"

#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "server/database.h"

#include "server/metrics/counters.h"
#include "server/ws/dto/RegisterCreatureRequestDto.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures ::ws {

class CreatureController : public oatpp::web::server::api::ApiController {
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

    // Helper function to add common HTTP attributes to a span
    void addHttpRequestAttributes(const std::shared_ptr<creatures::RequestSpan> &span,
                                  const std::shared_ptr<oatpp::web::protocol::http::incoming::Request> &request) {
        if (span && request) {
            span->setAttribute("http.method", std::string(request->getStartingLine().method.toString()));
            span->setAttribute("http.target", std::string(request->getStartingLine().path.toString()));

            // Add User-Agent if present (getHeader works directly)
            auto userAgent = request->getHeader("User-Agent");
            if (userAgent) {
                span->setAttribute("http.user_agent", std::string(userAgent));
            }

            // Add Content-Length if present
            auto contentLength = request->getHeader("Content-Length");
            if (contentLength) {
                span->setAttribute("http.request_content_length", std::string(contentLength));
            }

            // Add Host if present
            auto host = request->getHeader("Host");
            if (host) {
                span->setAttribute("http.host", std::string(host));
            }
            span->setAttribute("http.flavor", "1.1"); // Assuming HTTP/1.1 for oatpp
        }
    }

    ENDPOINT_INFO(getAllCreatures) {
        info->summary = "Get all of the creatures";

        info->addResponse<Object<CreaturesListDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/creature", getAllCreatures,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // Create a trace span for this request
        auto span = creatures::observability->createRequestSpan("GET /api/v1/creature", "GET", "api/v1/creature");
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "getAllCreatures");
            span->setAttribute("controller", "CreatureController");
        }

        auto result = m_creatureService.getAllCreatures(span);

        // Record success metrics in the span
        if (span) {
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(getCreature) {
        info->summary = "Get one creature by id";

        info->addResponse<Object<CreatureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_404, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");

        info->pathParams["creatureId"].description = "Creature ID in the form of an UUID";
    }
    ENDPOINT("GET", "api/v1/creature/{creatureId}", getCreature, PATH(String, creatureId),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        // RequestSpan only handles HTTP-level concerns
        auto span = creatures::observability->createRequestSpan("GET /api/v1/creature/{creatureId}", "GET",
                                                                "api/v1/creature/" + std::string(creatureId));
        addHttpRequestAttributes(span, request);

        creatures::metrics->incrementRestRequestsProcessed();

        if (span) {
            span->setAttribute("endpoint", "getCreature");
            span->setAttribute("controller", "CreatureController");
            span->setAttribute("creature.id", std::string(creatureId));
        }

        auto result = m_creatureService.getCreature(creatureId, span);

        if (span) {
            span->setHttpStatus(200);
        }

        return createDtoResponse(Status::CODE_200, result);
    }

    ENDPOINT_INFO(upsertCreature) {
        info->summary = "Upload or update a creature's JSON configuration";
        info->description =
            "Accepts raw creature JSON and upserts it to the database. "
            "All required fields must be present in the JSON (id, name, channel_offset, audio_channel, mouth_slot).";

        info->addResponse<Object<creatures::CreatureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/creature", upsertCreature, BODY_STRING(String, body),
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span = creatures::observability->createRequestSpan("POST /api/v1/creature", "POST", "api/v1/creature");
        addHttpRequestAttributes(span, request);

        debug("Upserting creature via POST /api/v1/creature");
        creatures::metrics->incrementRestRequestsProcessed();

        try {
            std::string creatureConfig = std::string(body);

            if (span) {
                span->setAttribute("endpoint", "upsertCreature");
                span->setAttribute("controller", "CreatureController");
                span->setAttribute("request.body_size", static_cast<int64_t>(creatureConfig.length()));
            }

            auto result = m_creatureService.upsertCreature(creatureConfig, span);

            if (span) {
                span->setAttribute("creature.id", std::string(result->id));
                span->setAttribute("creature.name", std::string(result->name));
                span->setHttpStatus(200);
            }

            // Schedule an event to invalidate the creature cache on the clients
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Creature);

            return createDtoResponse(Status::CODE_200, result);

        } catch (const std::exception &ex) {
            if (span) {
                span->recordException(ex);
                span->setHttpStatus(500);
            }

            error("Exception in upsertCreature: {}", ex.what());
            throw;
        }
    }

    ENDPOINT_INFO(registerCreature) {
        info->summary = "Register a creature with its universe assignment";
        info->description =
            "Called by controllers when they start up to register a creature and its current universe. "
            "The creature config from the controller's JSON file is the source of truth and will be upserted "
            "to the database. The universe assignment is stored in runtime memory only.";

        info->addResponse<Object<creatures::CreatureDto>>(Status::CODE_200, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
        info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
    }
    ENDPOINT("POST", "api/v1/creature/register", registerCreature,
             REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request)) {
        auto span = creatures::observability->createRequestSpan("POST /api/v1/creature/register", "POST",
                                                                "api/v1/creature/register");
        addHttpRequestAttributes(span, request);

        debug("----> Controller registering creature with universe assignment");

        // Read body manually instead of using BODY_STRING macro
        oatpp::String body = request->readBodyToString();

        // Debug: Log the raw request body
        debug("Raw request body size: {} bytes", body ? body->size() : 0);
        if (body && body->size() > 0) {
            debug("First 200 chars of body: {}",
                  std::string(body->data(), std::min(200UL, static_cast<size_t>(body->size()))));
        }

        creatures::metrics->incrementRestRequestsProcessed();

        // Parse the JSON manually for now
        Object<RegisterCreatureRequestDto> dto;
        try {
            auto json = nlohmann::json::parse(std::string(body));
            dto = RegisterCreatureRequestDto::createShared();
            dto->creature_config = json.value("creature_config", "");
            dto->universe = json.value("universe", 0);
        } catch (const std::exception &e) {
            std::string errorMessage = fmt::format("Failed to parse request body: {}", e.what());
            error(errorMessage);
            if (span) {
                span->setAttribute("error.message", errorMessage);
                span->setHttpStatus(400);
            }
            auto errorDto = StatusDto::createShared();
            errorDto->status = "ERROR";
            errorDto->code = 400;
            errorDto->message = errorMessage.c_str();
            return createDtoResponse(Status::CODE_400, errorDto);
        }

        try {
            std::string creatureConfig = std::string(dto->creature_config);

            if (span) {
                span->setAttribute("endpoint", "registerCreature");
                span->setAttribute("controller", "CreatureController");
                span->setAttribute("universe", static_cast<int64_t>(dto->universe));
                span->setAttribute("request.body_size", static_cast<int64_t>(creatureConfig.length()));
            }

            auto result = m_creatureService.registerCreature(creatureConfig, dto->universe, span);

            if (span) {
                span->setAttribute("creature.id", std::string(result->id));
                span->setAttribute("creature.name", std::string(result->name));
                span->setHttpStatus(200);
            }

            // Schedule an event to invalidate the creature cache on the clients
            scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Creature);

            return createDtoResponse(Status::CODE_200, result);

        } catch (const std::exception &ex) {
            if (span) {
                span->recordException(ex);
                span->setHttpStatus(500);
            }

            error("Exception in registerCreature: {}", ex.what());
            throw;
        }
    }
};

} // namespace creatures::ws

#include OATPP_CODEGEN_END(ApiController)