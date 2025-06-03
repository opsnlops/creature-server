#pragma once

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/incoming/Request.hpp>


#include "oatpp/web/protocol/http/Http.hpp"
#include "oatpp/core/macro/component.hpp"
#include "oatpp/core/Types.hpp"
#include "oatpp/web/server/HttpConnectionHandler.hpp"
#include "oatpp/web/server/AsyncHttpConnectionHandler.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"


#include <nlohmann/json.hpp>
using json = nlohmann::json;

#include "server/database.h"

#include "server/ws/service/CreatureService.h"
#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObservabilityManager> observability;
}

#include OATPP_CODEGEN_BEGIN(ApiController)

namespace creatures :: ws {

    class CreatureController : public oatpp::web::server::api::ApiController {
    public:
        CreatureController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper)):
            oatpp::web::server::api::ApiController(objectMapper) {}
    private:
        CreatureService m_creatureService;
    public:

        static std::shared_ptr<CreatureController> createShared(
                OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                objectMapper)
        ) {
            return std::make_shared<CreatureController>(objectMapper);
        }

        // Helper function to add common HTTP attributes to a span
        void addHttpRequestAttributes(
            const std::shared_ptr<creatures::RequestSpan>& span,
            const std::shared_ptr<oatpp::web::protocol::http::incoming::Request>& request)
        {
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
                 REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request))
        {
            // Create a trace span for this request
            auto span = creatures::observability->createRequestSpan(
                "GET /api/v1/creature", "GET", "api/v1/creature"
            );
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
        ENDPOINT("GET", "api/v1/creature/{creatureId}", getCreature,
                 PATH(String, creatureId),
                 REQUEST(std::shared_ptr<oatpp::web::protocol::http::incoming::Request>, request))
        {
            // RequestSpan only handles HTTP-level concerns
            auto span = creatures::observability->createRequestSpan(
                "GET /api/v1/creature/{creatureId}", "GET",
                "api/v1/creature/" + std::string(creatureId)
            );
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
            info->summary = "Update or insert a creature";

            info->addResponse<Object<creatures::CreatureDto>>(Status::CODE_200, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_400, "application/json; charset=utf-8");
            info->addResponse<Object<StatusDto>>(Status::CODE_500, "application/json; charset=utf-8");
        }
        ENDPOINT("POST", "api/v1/creature", upsertCreature,
                 REQUEST(std::shared_ptr<IncomingRequest>, request))
        {
            // Span for the upsert operation
            auto span = creatures::observability->createRequestSpan(
                "POST /api/v1/creature", "POST", "api/v1/creature"
            );
            addHttpRequestAttributes(span, request);

            debug("new creature configuration uploaded via REST API");
            creatures::metrics->incrementRestRequestsProcessed();

            try {
                auto requestAsString = std::string(request->readBodyToString());
                trace("request was: {}", requestAsString);

                if (span) {
                    span->setAttribute("endpoint", "upsertCreature");
                    span->setAttribute("controller", "CreatureController");
                    span->setAttribute("request.body_size", static_cast<int64_t>(requestAsString.length()));
                }

                auto result = m_creatureService.upsertCreature(requestAsString, span);

                if (span) {
                    span->setAttribute("creature.id", std::string(result->id));
                    span->setAttribute("creature.name", std::string(result->name)); // Assuming creatures have a 'name'
                    span->setHttpStatus(200);
                }

                // Schedule an event to invalidate the creature cache on the clients
                scheduleCacheInvalidationEvent(CACHE_INVALIDATION_DELAY_TIME, CacheType::Creature);

                return createDtoResponse(Status::CODE_200, result);

            } catch (const std::exception& ex) {
                if (span) {
                    span->recordException(ex);
                    span->setHttpStatus(500);
                }

                error("Exception in upsertCreature: {}", ex.what());
                throw;
            }
        }

    };

}

#include OATPP_CODEGEN_END(ApiController)