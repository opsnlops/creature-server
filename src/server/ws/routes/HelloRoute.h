
#pragma once

#include "server/ws/WebSocketServer.h"

#include "BaseRoute.h"


namespace creatures :: ws {

    class HelloRoute : public BaseRoute {

    public:
        explicit HelloRoute(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering /hello route via HelloRoute");

            app.get("/hello", [this](auto *res, auto *req) {
                logger->debug("Said hello!");
                WebSocketServer::sendResponse(res, {HttpStatus::OK, "Hellorld!"});
            });
        }

    };


}