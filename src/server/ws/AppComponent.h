
#pragma once

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <oatpp/network/tcp/server/ConnectionProvider.hpp>
#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include <oatpp/core/macro/component.hpp>

#include <oatpp-websocket/ConnectionHandler.hpp>

#include <CreatureVoices.h>

#include "ErrorHandler.h"
#include "SwaggerComponent.h"

#include "server/config/Configuration.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/websocket/ClientCafe.h"
#include "util/MessageQueue.h"
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
        // This doesn't work very well and I haven't figured out why yet
        // auto logger = creatures::makeLogger("web-server", spdlog::level::debug);
        // return logger;

        auto _appLogger = spdlog::stdout_color_mt("web-server");
        _appLogger->set_level(spdlog::level::debug);
        return _appLogger;
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
        return oatpp::network::tcp::server::ConnectionProvider::createShared(
            {"0.0.0.0", 8000, oatpp::network::Address::IP_4});
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

        auto connectionHandler = oatpp::web::server::HttpConnectionHandler::createShared(router);
        connectionHandler->setErrorHandler(std::make_shared<ErrorHandler>(objectMapper));
        return connectionHandler;
    }());

    OATPP_CREATE_COMPONENT(std::shared_ptr<ClientCafe>, cafe)([] { return std::make_shared<ClientCafe>(); }());

    /**
     * Register the voice service
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<creatures::voice::CreatureVoices>, creatureVoices)([] {
        return std::make_shared<creatures::voice::CreatureVoices>(config->getVoiceApiKey());
    }());

    /**
     * Create the MessageProcessor
     */
    OATPP_CREATE_COMPONENT(std::shared_ptr<creatures::ws::MessageProcessor>, messageProcessor)([] {
        auto _messageProcessor = std::make_shared<creatures::ws::MessageProcessor>();
        return _messageProcessor;
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