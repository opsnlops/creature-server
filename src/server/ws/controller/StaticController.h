
#pragma once

#include <fmt/format.h>

#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/web/server/api/ApiController.hpp>

#include "Version.h"

#include "server/metrics/counters.h"
#include "server/transport/ApiDocumentation.h"
#include "server/ws/controller/ControllerUtils.h"
#include "server/ws/controller/HttpResponseHelpers.h"

namespace creatures ::ws {

#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

class StaticController : public oatpp::web::server::api::ApiController, public HttpResponseHelpers<StaticController> {
  public:
    StaticController(const std::shared_ptr<ObjectMapper> &objectMapper)
        : oatpp::web::server::api::ApiController(objectMapper) {}

  public:
    static std::shared_ptr<StaticController>
    createShared(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                                 objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<StaticController>(objectMapper);
    }

    ENDPOINT("GET", "/", root, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /", "GET", "/", "root", "StaticController", request, [&](const auto &span) {
            const char *html = "<html lang='en'>"
                               "  <head>"
                               "    <meta charset=utf-8/>"
                               "  </head>"
                               "  <body>"
                               "    <h1>April's Creature Workshop</h1>"
                               "    <p>This is the server that controls everything. <a href='/api/docs'>Browse "
                               "the API</a>!</p>"
                               "  </body>"
                               "</html>";
            auto response = createResponse(Status::CODE_200, html);
            std::string version = fmt::format("Creature-Server/{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR,
                                              CREATURE_SERVER_VERSION_MINOR, CREATURE_SERVER_VERSION_PATCH);
            response->putHeader(Header::CONTENT_TYPE, "text/html");
            response->putHeader(Header::SERVER, oatpp::String(version));
            response->putHeader("All-The-Birds-Sing-Words", oatpp::String("yes"));
            response->putHeader("And-The-Flowers-Croon", oatpp::String("of course"));
            if (span)
                span->setHttpStatus(200);
            return response;
        });
    }

    ENDPOINT_INFO(health) {
        info->summary = "Returns OK if the server is healthy";
        info->addTag("System");

        info->addResponse<oatpp::String>(Status::CODE_200, "application/json; charset=utf-8");
    }
    ENDPOINT("GET", "api/v1/health", health, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/v1/health", "GET", "api/v1/health", "health", "StaticController", request,
                           [&](const auto &span) { return okStatus(span, Status::CODE_200, "Server is operational"); });
    }

    ENDPOINT("GET", "/api/docs", apiDocs, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint(
            "GET /api/docs", "GET", "/api/docs", "apiDocs", "StaticController", request, [&](const auto &span) {
                auto response = createResponse(Status::CODE_200, oatpp::String(transport::apiBrowserHtml().data(),
                                                                               transport::apiBrowserHtml().size()));
                response->putHeader(Header::CONTENT_TYPE, "text/html; charset=utf-8");
                response->putHeader("Content-Security-Policy", "default-src 'self'; style-src 'unsafe-inline'; "
                                                               "script-src 'unsafe-inline'; connect-src 'self'");
                if (span)
                    span->setHttpStatus(200);
                return response;
            });
    }

    ENDPOINT("GET", "/api/openapi.json", openApi, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        return runEndpoint("GET /api/openapi.json", "GET", "/api/openapi.json", "openApi", "StaticController", request,
                           [&](const auto &span) {
                               const auto &document = transport::openApiDocument();
                               auto response = createResponse(Status::CODE_200, oatpp::String(document));
                               response->putHeader(Header::CONTENT_TYPE, "application/json; charset=utf-8");
                               if (span)
                                   span->setHttpStatus(200);
                               return response;
                           });
    }
};

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen

} // namespace creatures::ws
