
#pragma once

#include <stdexcept>

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include <oatpp/core/macro/component.hpp>

#include <oatpp-websocket/ConnectionHandler.hpp>

#include "ErrorHandler.h"
#include "RequestBodyDrain.h"
#include "SwaggerComponent.h"

#include "server/config.h"
#include "server/config/Configuration.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/websocket/ClientCafe.h"
#include "util/MessageQueue.h"
#include "util/environment.h"
#include "util/loggingUtils.h"

namespace creatures {
extern std::shared_ptr<creatures::Configuration> config;
}

namespace creatures ::ws {

class AppComponent {
  public:
    SwaggerComponent swaggerComponent;

    /**
     *  Create Logger component
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger)([] {
        auto logger = spdlog::stdout_color_mt("web-server");
        logger->set_level(spdlog::level::debug);
        return logger;
    }());

    /**
     * Create ObjectMapper component to serialize/deserialize DTOs in Controller's API
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper)([] {
        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        objectMapper->getDeserializer()->getConfig()->allowUnknownFields = false;
        return objectMapper;
    }());

    /**
     *  Create ConnectionProvider component which listens on the port
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, serverConnectionProvider)([] {
        const auto port = environmentToInt(SERVER_PORT_ENV, DEFAULT_SERVER_PORT);
        if (port < 1 || port > 65535) {
            throw std::runtime_error("SERVER_PORT must be between 1 and 65535");
        }
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            {"0.0.0.0", static_cast<v_uint16>(port), oatpp::network::Address::IP_4});
    }());

    /**
     *  Create Router component
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>,
                           httpRouter)([] { return oatpp::web::server::HttpRouter::createShared(); }());

    /**
     *  Create ConnectionHandler component which uses Router component to route requests
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)("rest", [] {
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router); // get Router component
        OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>,
                        objectMapper); // get ObjectMapper component

        // Swap in a body decoder that records whether the endpoint read the request
        // body, so the drain interceptor below can discard unread bodies instead of
        // letting them poison the next request on a keep-alive connection (issue #142).
        auto components = std::make_shared<oatpp::web::server::HttpProcessor::Components>(router);
        components->bodyDecoder = std::make_shared<TrackingBodyDecoder>();

        auto connectionHandler = std::make_shared<oatpp::web::server::HttpConnectionHandler>(components);
        connectionHandler->setErrorHandler(std::make_shared<ErrorHandler>(objectMapper));
        connectionHandler->addRequestInterceptor(std::make_shared<RequestBodyDrainRequestInterceptor>());
        connectionHandler->addResponseInterceptor(std::make_shared<RequestBodyDrainResponseInterceptor>());
        return connectionHandler;
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<ClientCafe>, cafe)([] { return std::make_shared<ClientCafe>(); }());

    /**
     * Create the MessageProcessor
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<creatures::ws::MessageProcessor>, messageProcessor)([] {
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
        return std::make_shared<creatures::ws::MessageProcessor>(appLogger);
    }());

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

    /**
     *  Create websocket connection handler
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>,
                           websocketConnectionHandler)("websocket" /* qualifier */, [] {
        OATPP_COMPONENT(std::shared_ptr<ClientCafe>,
                        cafe); // This isn't a shadowed variable. The macros make it look like it is.
        auto wsConnectionHandler = oatpp::websocket::ConnectionHandler::createShared();
        wsConnectionHandler->setSocketInstanceListener(cafe);
        return wsConnectionHandler;
    }());

#pragma GCC diagnostic pop
};

} // namespace creatures::ws
