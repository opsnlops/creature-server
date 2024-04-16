
#pragma once

#include "server/ws/WebSocketServer.h"

#include "server/ws/routes/BaseRoute.h"


namespace creatures :: ws {

    class CreateCreature : public BaseRoute {

    public:
        explicit CreateCreature(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering the dummy /api/v1/creature route via CreateCreature");

            /*
             * We don't do POSTs this way. That's what the WebSocket is for.
             */
            app.post("/api/v1/creature", [this](auto *res, auto *req) {
                logger->warn("Something with a user-agent of '{}' tried to POST to /api/v1/creature?!", req->getHeader("user-agent"));
                WebSocketServer::sendResponse(res, {HttpStatus::NotImplemented, "Use the WebSocket to create creatures!"});
            });
        }

    };


}