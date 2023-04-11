

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"

using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

extern creatures::Database *db;

namespace creatures {

    Status CreatureServerImpl::SearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply) {
        debug("calling handleSearchCreatures()");
        return handleSearchCreatures(context, request, reply);
    }

    Status CreatureServerImpl::CreateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) {
        debug("hello from save");
        return handleSave(context, creature, reply);
    }

    Status CreatureServerImpl::GetCreature(ServerContext *context, const CreatureId *id, Creature *reply) {
        debug("calling getCreature()");
        return handleGetCreature(context, id, reply);
    }

    Status CreatureServerImpl::ListCreatures(ServerContext *context, const CreatureFilter *filter,
                                             ListCreaturesResponse *response) {
        debug("calling listCreatures()");
        return handleListCreatures(context, filter, response);
    }

    Status CreatureServerImpl::GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
                                               GetAllCreaturesResponse *response) {
        debug("calling handleGetAllCreatures()");
        return handleGetAllCreatures(context, filter, response);
    }

    Status handleSearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply) {

        debug("handleGetCreature() time!");

        grpc::Status status;

        try {
            db->searchCreatures(request, reply);
            debug("creature {} found in DB!", request->name());
            status = grpc::Status(grpc::StatusCode::OK,
                                  fmt::format("✅ Searched for creature name '{}' successfully!", request->name()));
            return status;

        }
        catch (const creatures::CreatureNotFoundException &e) {
            info("creature {} not found", request->name());
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature name '{}' not found", request->name()));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while looking for a creature: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "A data formatting error occurred while looking for this creature");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty name was passed into searchCreatures()");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature name must be supplied"));
            return status;
        }
    }

    Status handleListCreatures(ServerContext *context, const CreatureFilter *filter, ListCreaturesResponse *response) {
        debug("called handleListCreatures()");
        grpc::Status status;

        db->listCreatures(filter, response);
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("✅🦖Returned all creatures IDs and names"));
        return status;
    }

    Status
    handleGetAllCreatures(ServerContext *context, const CreatureFilter *filter, GetAllCreaturesResponse *response) {
        debug("called handleListCreatures()");
        grpc::Status status;

        db->getAllCreatures(filter, response);
        status = grpc::Status(grpc::StatusCode::OK,
                              fmt::format("🐰🐻🦁 Returned all creatures!"));
        return status;
    }


    Status handleGetCreature(ServerContext *context, const CreatureId *id, Creature *reply) {

        debug("handleGetCreature() time!");

        grpc::Status status;

        try {
            db->getCreature(id, reply);
            debug("creature found in DB on get!");
            status = grpc::Status(grpc::StatusCode::OK,
                                  fmt::format("✅ Got creature by id successfully!"));
            return status;

        }
        catch (const creatures::CreatureNotFoundException &e) {
            info("creature id not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature id not found"));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while getting a creature by id: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "A data formatting error occurred while looking for a creature by id");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty name was passed into getCreature()");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }
    }

    Status handleSave(ServerContext *context, const Creature *request, DatabaseInfo *reply) {

        debug("asking the server to save maybe?");
        return db->createCreature(request, reply);
    }
}