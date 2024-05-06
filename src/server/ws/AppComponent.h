
#pragma once

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>





#include <oatpp/web/server/HttpConnectionHandler.hpp>
#include <oatpp/web/server/HttpRouter.hpp>
#include <oatpp/network/tcp/server/ConnectionProvider.hpp>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include <oatpp/core/macro/component.hpp>

#include <oatpp-websocket/ConnectionHandler.hpp>

#include "SwaggerComponent.h"
#include "ErrorHandler.h"

#include "server/ws/websocket/ClientCafe.h"
#include "util/loggingUtils.h"
#include "util/MessageQueue.h"

namespace creatures :: ws {

    class AppComponent {
    public:


        SwaggerComponent swaggerComponent;

        /**
         *  Create Logger component
         */
        OATPP_CREATE_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger)([] {
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
            return oatpp::network::tcp::server::ConnectionProvider::createShared({"0.0.0.0", 8000, oatpp::network::Address::IP_4});
        }());

        /**
         *  Create Router component
         */
        OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, httpRouter)([] {
            return oatpp::web::server::HttpRouter::createShared();
        }());

        /**
         *  Create ConnectionHandler component which uses Router component to route requests
         */
        OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, serverConnectionHandler)("rest", [] {

            OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router); // get Router component
            OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, objectMapper); // get ObjectMapper component

            auto connectionHandler = oatpp::web::server::HttpConnectionHandler::createShared(router);
            connectionHandler->setErrorHandler(std::make_shared<ErrorHandler>(objectMapper));
            return connectionHandler;

        }());

        OATPP_CREATE_COMPONENT(std::shared_ptr<ClientCafe>, cafe)([] {
            return std::make_shared<ClientCafe>();
        }());


#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

        /**
         *  Create websocket connection handler
         */
        OATPP_CREATE_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, websocketConnectionHandler)(
                "websocket" /* qualifier */, [] {
                    OATPP_COMPONENT(std::shared_ptr<ClientCafe>,
                                    cafe); // This isn't a shadowed variable. The macros make it look like it is.
                    auto wsConnectionHandler = oatpp::websocket::ConnectionHandler::createShared();
                    wsConnectionHandler->setSocketInstanceListener(cafe);
                    return wsConnectionHandler;
                }());

#pragma GCC diagnostic pop

    };

}