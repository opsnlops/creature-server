
#pragma once

#include <fmt/format.h>

#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>

#include "Version.h"

#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}

namespace creatures ::ws {


#include OATPP_CODEGEN_BEGIN(ApiController) //<- Begin Codegen

class StaticController : public oatpp::web::server::api::ApiController {
public:
    StaticController(const std::shared_ptr<ObjectMapper> &objectMapper)
            : oatpp::web::server::api::ApiController(objectMapper) {}

public:

    static std::shared_ptr<StaticController> createShared(
            OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                            objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<StaticController>(objectMapper);
    }

    ENDPOINT("GET", "/", root) {
        const char *html =
                "<html lang='en'>"
                "  <head>"
                "    <meta charset=utf-8/>"
                "  </head>"
                "  <body>"
                "    <h1>April's Creature Workshop</h1>"
                "    <p>This is the server that controls everything. <a href='swagger/ui'>Checkout the Swagger-UI page</a>!</p>"
                "  </body>"
                "</html>";
        auto response = createResponse(Status::CODE_200, html);
        std::string version = fmt::format("Creature-Server/{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR, CREATURE_SERVER_VERSION_PATCH);
        response->putHeader(Header::CONTENT_TYPE, "text/html");
        response->putHeader(Header::SERVER, oatpp::String(version));
        response->putHeader("All-The-Birds-Sing-Words", oatpp::String("yes"));
        response->putHeader("And-The-Flowers-Croon", oatpp::String("of course"));
        creatures::metrics->incrementRestRequestsProcessed();
        return response;
    }

};

#include OATPP_CODEGEN_END(ApiController) //<- End Codegen


}