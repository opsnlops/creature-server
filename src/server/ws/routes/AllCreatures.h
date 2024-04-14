
#pragma once


#include "model/Creature.h"
#include "server/database.h"
#include "server/ws/WebSocketServer.h"

#include "BaseRoute.h"


namespace creatures :: ws {

    extern std::shared_ptr<Database> db;

    class AllCreatures : public BaseRoute {

    public:
        explicit AllCreatures(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering /creatures route via AllCreatures");

            app.get("/creatures", [this](auto *res, auto */*req*/) {

                logger->debug("Getting all creatures");

                auto creature1 = creatures::Creature{"1234", "Beaky", 2, 1, "This is a note!"};
                auto creature2 = creatures::Creature{"4567", "Mango 😍", 2, 5, "This is a note, too"};

                std::vector<creatures::Creature> creatures = {creature1, creature2};
                WebSocketServer::sendResponse(res, {HttpStatus::OK, creatures});

            });
        }


    };


}
