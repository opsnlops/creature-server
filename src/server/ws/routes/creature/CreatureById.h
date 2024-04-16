
#pragma once


#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/ws/WebSocketServer.h"

#include "server/ws/routes/BaseRoute.h"

/**
 * Get one creature by its ID

 */

namespace creatures {
    extern std::shared_ptr<Database> db;
}

namespace creatures :: ws {

    class CreatureById : public BaseRoute {

    public:
        explicit CreatureById(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App& app) override {

            logger->debug("Registering /api/v1/creature/:id route via CreatureById");

            app.get("/api/v1/creature/:id", [this](auto *res, auto *req) {

                // Convert the std::string_view to a std::string
                std::string id = std::string(req->getParameter("id"));
                this->logger->debug("got id: {}", id);

                if(id.empty()) {
                    WebSocketServer::sendResponse(res, {HttpStatus::BadRequest, "Missing ID parameter"});
                    this->logger->warn("missing id parameter on /api/v1/creature/:id");
                    return;
                }

                try {

                    auto creature = creatures::db->getCreature(id);
                    this->logger->debug("found creature: {}", creature.name);

                    WebSocketServer::sendResponse(res, {HttpStatus::OK, creature});
                    this->logger->debug("got creature by id: {}", id);
                }
                catch (const creatures::InternalError &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, e.what()});
                    this->logger->error("Internal error while getting creature by id: {}", e.what());
                }
                catch (const creatures::DataFormatException &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, e.what()});
                    this->logger->error("Data format exception while getting creature by id: {}", e.what());
                }
                catch (const creatures::InvalidArgumentException &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::BadRequest, e.what()});
                    this->logger->error("Invalid argument for id: {}", e.what());
                }
                catch (const creatures::NotFoundException &e) {
                    WebSocketServer::sendResponse(res, {HttpStatus::NotFound, e.what()});
                    this->logger->info("creature id not found: {}", id);
                }
                catch (...) {
                    this->logger->error("Caught an unknown exception.");
                    WebSocketServer::sendResponse(res, {HttpStatus::InternalServerError, "Unknown error"});
                }
            });
        }
    };


}
