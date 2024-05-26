
#pragma once


#include <oatpp/web/server/api/ApiController.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/macro/codegen.hpp>
#include <oatpp/core/macro/component.hpp>

#include <oatpp/network/ConnectionHandler.hpp>

#include <oatpp-websocket/Handshaker.hpp>

#include "server/metrics/counters.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}



namespace creatures :: ws {


#include OATPP_CODEGEN_BEGIN(ApiController) //<-- codegen begin

/**
 * Controller with WebSocket-connect endpoint.
 */
class WebSocketController : public oatpp::web::server::api::ApiController {
private:
    OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, websocketConnectionHandler, "websocket");
public:
    WebSocketController(OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
            : oatpp::web::server::api::ApiController(objectMapper)
    {}
public:


    static std::shared_ptr<WebSocketController> createShared(
            OATPP_COMPONENT(std::shared_ptr<ObjectMapper>,
                            objectMapper) // Inject objectMapper component here as default parameter
    ) {
        return std::make_shared<WebSocketController>(objectMapper);
    }


    ENDPOINT_INFO(ws) {
        info->summary = "WebSocket endpoint";
    }
    ENDPOINT("GET", "api/v1/websocket", ws, REQUEST(std::shared_ptr<IncomingRequest>, request)) {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
        appLogger->info("WebSocket connection received");
        creatures::metrics->incrementWebsocketConnectionsProcessed();
        return oatpp::websocket::Handshaker::serversideHandshake(request->getHeaders(), websocketConnectionHandler);
    };

};

#include OATPP_CODEGEN_END(ApiController) //<-- codegen end

} // creatures :: ws
