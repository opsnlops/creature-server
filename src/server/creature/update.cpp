
#include <string>

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server.pb.h"

#include "server/config.h"
#include "server/creature-server.h"
#include "server/database.h"

#include <grpcpp/grpcpp.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <mongocxx/client.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::basic::kvp;

namespace creatures {

    extern std::shared_ptr<Database> db;


    Status CreatureServerImpl::UpdateCreature(grpc::ServerContext *context, const server::Creature *creature,
                                              DatabaseInfo *response) {

        debug("trying to update a creature");

        try {
            db->updateCreature(creature);
            std::string statusMessage = fmt::format("✅ Creature updated in database! Name: {}", creature->name());
            response->set_message(statusMessage);
            info(statusMessage);
            return {grpc::StatusCode::OK, statusMessage};

        }
        catch (const InvalidArgumentException &e) {
            std::string  errorMessage = fmt::format("CreatureID was empty on updateCreature()");
            error(errorMessage);
            response->set_message("⛔️ A creature ID must be supplied");
            response->set_help("creatureID cannot be empty");
            return {grpc::StatusCode::INVALID_ARGUMENT, "⛔️️ A creature ID must be supplied"};

        }
        catch (const NotFoundException &e) {
            std::string errorMessage = fmt::format("⚠️ No creature with ID '{}' found",
                                                   bsoncxx::oid(creature->_id()).to_string());
            debug(errorMessage);
            response->set_message(errorMessage);
            response->set_help("Try another ID! 😅");
            return {grpc::StatusCode::NOT_FOUND, errorMessage};

        }
        catch (const DataFormatException &e) {
            std::string errorMessage = fmt::format("Unable to encode creature into BSON: {}", e.what());
            critical(errorMessage);
            response->set_message("Unable to encode creature into BSON");
            response->set_help(e.what());
            return {grpc::StatusCode::INTERNAL,errorMessage};

        }
        catch (const InternalError &e) {
            std::string errorMessage = fmt::format("MongoDB error while updating a creature: {}", e.what());
            critical(errorMessage);
            response->set_message("MongoDB error while updating creature");
            response->set_help(e.what());
            return {grpc::StatusCode::INTERNAL, errorMessage};
        }
        catch (...) {
            std::string errorMessage = "🚨 An unknown error happened while updating a creature 🚨";
            error(errorMessage);
            response->set_message("An unknown error happened while updating a creature");
            response->set_help("Default catch hit?");
            return {grpc::StatusCode::INTERNAL, errorMessage};
        }

    }


    /**
     * TODO: I bet this can be generalized
     *
     * @param creature
     */
    void Database::updateCreature(const server::Creature *creature) {

        debug("attempting to update a creature in the database");

        // Error checking
        if (creature->_id().empty()) {
            error("an empty creatureId was passed into updateCreature()");
            throw InvalidArgumentException("an empty creatureId was passed into updateCreature()");
        }

        //debug("creature ID for update: {}", bytesToString(creature->_id()));

        auto collection = getCollection(CREATURES_COLLECTION);
        trace("collection obtained");

        try {

            // Convert the creatureId to a proper oid
            bsoncxx::oid creatureId = bsoncxx::oid(creature->_id().data(), bsoncxx::oid::k_oid_length);

            // Create a filter for just this one creature
            bsoncxx::builder::stream::document filter_builder{};
            filter_builder << "_id" << creatureId;

            auto doc_view = creatureToBson(creature, false);
            auto result = collection.replace_one(filter_builder.view(), doc_view.view());

            if (result) {
                if (result->matched_count() > 1) {

                    // Whoa, this should never happen
                    std::string errorMessage = fmt::format(
                            "more than one document updated at once in updateCreature()!! Count: {}",
                            result->matched_count());
                    critical(errorMessage);
                    throw InternalError(errorMessage);

                } else if (result->matched_count() == 0) {

                    // We didn't update anything
                    std::string errorMessage = fmt::format("Update to update creature. Reason: creature {} not found",
                                                           creatureId.to_string());
                    info(errorMessage);
                    throw NotFoundException(errorMessage);
                }

                // Hooray, we only did one. :)
                info("✅ creature {} updated", creatureId.to_string());
                return;

            }

            // Something went wrong
            std::string errorMessage = fmt::format("Unknown errors while updating creature {} (result wasn't)",
                                                   creatureId.to_string());
            throw InternalError(errorMessage);
        }
        catch (const bsoncxx::exception &e) {
            error("BSON exception while updating a creature: {}", e.what());
            throw DataFormatException(fmt::format("BSON exception while updating a creature: {}", e.what()));
        }
        catch (const mongocxx::exception &e) {
            error("MongoDB exception while updating a creature: {}", e.what());
            throw InternalError(fmt::format("MongoDB exception while updating a creature: {}", e.what()));
        }
    }


}
