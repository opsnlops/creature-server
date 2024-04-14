
#pragma once


#include "model/Creature.h"
#include "server/database.h"
#include "server/ws/WebSocketServer.h"

#include "BaseRoute.h"


namespace creatures :: ws {


    extern std::shared_ptr<Database> db;

    class CreatureById : public BaseRoute {

    public:
        explicit CreatureById(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering /creature/:id route via CreatureById");

            app.get("/creature/:id", [](auto *res, auto *req) {

                std::string response = fmt::format("I would have gotten everything if I could! You wanted to see {}",
                                                   req->getParameter("id"));
                WebSocketServer::sendResponse(res, {HttpStatus::NotImplemented, response});

            });
        }


    };


}
