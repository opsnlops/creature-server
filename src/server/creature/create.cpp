
#include "server/config.h"

#include <string>
#include "spdlog/spdlog.h"

#include "server.pb.h"

#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "util/helpers.h"

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>



#include <bsoncxx/builder/stream/document.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;

    Status CreatureServerImpl::CreateCreature(ServerContext *context, const server::Creature *creature, DatabaseInfo *reply) {debug("hello from save");

        debug("attempting to save a new creature");
        try {
            db->gRPCcreateCreature(creature, reply);
            return {grpc::StatusCode::OK, "✅ Created new creature"};
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("Invalid argument exception while creating a creature: {}", e.what());
            return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
        }
        catch (const creatures::InternalError &e) {
            error("Internal error while creating a creature: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch (...) {
            error("Unknown error while creating a creature");
            return {grpc::StatusCode::INTERNAL, "Unknown error"};
        }
    }


    void Database::gRPCcreateCreature(const server::Creature *creature, DatabaseInfo *reply) {

        info("attempting to save a creature in the database");

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection made");

        grpc::Status status;

        debug("name: {}", creature->name().c_str());
        try {
            auto doc_value = gRPCcreatureToBson(creature, true);
            trace("doc_value made");
            collection.insert_one(doc_value.view());
            trace("run_command done");

            info("saved creature in the database");

            reply->set_message("created new creature in the database");

        }
        catch (const mongocxx::exception &e) {
            // Was this an attempt to make a duplicate creature?
            if (e.code().value() == 11000) {
                std::string error_message = fmt::format("attempted to insert a duplicate Creature in the database for id {}",
                                                        creature->_id());

                reply->set_message("Unable to create new creature");
                reply->set_help(fmt::format("ID {} already exists", creature->_id()));
                throw creatures::InvalidArgumentException(error_message);

            } else {

                std::string error_message = fmt::format("Error in the database while adding a creature: {} ({})",
                                                        e.what(), e.code().value());
                critical(error_message);
                reply->set_message(error_message);
                reply->set_help(e.code().message());
                throw creatures::InternalError(error_message);
            }
        }

        catch (...) {
            std::string error_message = "Unknown error while adding a creature to the database";
            critical(error_message);
            reply->set_message(error_message);
            throw creatures::InternalError(error_message);
        }

    }



    /**
     * Create a new creature in the database. The ID is ignored and a new one is generated.
     *
     * @param creature The creature to create
     * @return a new creature with the ID set
     */
    creatures::Creature Database::createCreature(creatures::Creature creature) {

        info("attempting to create a new a creature in the database");

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection located");

        if(creature.name.empty()) {
            std::string error_message = "Creature name is empty";
            info(error_message);
            throw creatures::DataFormatException(error_message);
        }

        if(creature.audio_channel < 0) {
            std::string error_message = "Creature audio_channel is less than 0";
            info(error_message);
            throw creatures::DataFormatException(error_message);
        }

        if(creature.channel_offset < 0) {
            std::string error_message = "Creature channel_offset is less than 0";
            info(error_message);
            throw creatures::DataFormatException(error_message);
        }

        if(creature.channel_offset > 511) {
            std::string error_message = "Creature channel_offset is greater than 511";
            info(error_message);
            throw creatures::DataFormatException(error_message);
        }


        bsoncxx::oid newOid = generateNewOid();
        std::string newOidAsString = bytesToString(newOid.bytes());
        debug("the new creature ID is: {}", newOidAsString);

        creature.id = newOidAsString;

        debug("name: {}", creature.name);
        try {
            auto doc_value = creatureToBson(creature);
            trace("doc_value made");
            collection.insert_one(doc_value.view());
            trace("run_command done");

            info("created a new creature in the database with ID {}", creature.id);
            return creature;

        }
        catch (const mongocxx::exception &e) {


            /*
             * Whoa whoa whoa. Something went really wrong if there's a duplicate creature! This OID should be
             * freshly created and unique!
             */
            if (e.code().value() == 11000) {
                std::string error_message = fmt::format("attempted to insert a duplicate Creature in the database for id {}",
                                                        creature.id);
                error(error_message);
                throw creatures::DuplicateFoundError(error_message);

            } else {

                std::string error_message = fmt::format("Error in the database while adding a creature: {} ({})",
                                                        e.what(), e.code().value());
                error(error_message);
                throw creatures::InternalError(error_message);
            }

        }

        catch (...) {
            std::string error_message = "Unknown error while adding a creature to the database";
            critical(error_message);
            throw creatures::InternalError(error_message);
        }

    }





}