
#pragma once


#include "model/Creature.h"
#include "model/SortBy.h"
#include "server/database.h"
#include "server/ws/WebSocketServer.h"

#include "server/ws/routes/BaseRoute.h"
#include "exception/exception.h"


/**
 * Get all of the creatures in a list
 */

namespace creatures {
    extern std::shared_ptr<Database> db;
}

namespace creatures :: ws {

    class AllCreatures : public BaseRoute {

    public:
        explicit AllCreatures(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering GET for /api/v1/creature route via AllCreatures");

            app.get("/api/v1/creature", [](auto *res, auto */*req*/) {

                try {
                    auto creatures = creatures::db->getAllCreatures(creatures::SortBy::name, true);
                    WebSocketServer::sendResponse(res, {HttpStatus::OK, creatures});
                }
                catch (const creatures::InternalError &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, e.what()});
                }
                catch (const creatures::DataFormatException &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, e.what()});
                }
                catch (...) {
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, "Unknown error"});
                }

            });
        }


    };


}
