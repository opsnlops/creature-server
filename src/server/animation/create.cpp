
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"




namespace creatures {

    extern std::shared_ptr<Database> db;

    /**
     * Save a new animation in the database
     *
     * @param context the `ServerContext` of this request
     * @param animation an `Animation` to save
     * @param reply a `DatabaseInfo` that will be filled out for the reply
     * @return state of the request
     */
    Status CreatureServerImpl::gRPCCreateAnimation(ServerContext *context,
                                               const Animation *animation,
                                               DatabaseInfo *reply) {

        debug("creating a new animation in the database");
        try {
            db->createAnimation(animation, reply);
            info("created new animation in the database");
            return {grpc::StatusCode::OK, "✅ Created new animation in the database"};
        }
        catch (const creatures::DuplicateFoundError &e) {
            debug("Duplicate found while creating a new animation: {}", e.what());
            return {grpc::StatusCode::ALREADY_EXISTS, e.what()};
        }
        catch (const creatures::DataFormatException &e) {
            error("Data format error while creating a new animation: {}", e.what());
            return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
        }
        catch (const creatures::DatabaseError &e) {
            error("Database error while creating a new animation: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch (const creatures::InternalError &e) {
            error("Internal error while creating a new animation: {}", e.what());
            return {grpc::StatusCode::INTERNAL, e.what()};
        }
        catch (...) {
            error("Unknown error while creating a new animation");
            return {grpc::StatusCode::INTERNAL, "Unknown error"};
        }
    }

    /**
     * Create a new animation in the database
     *
     * @param animation the `Animation` to save
     * @param reply Information about the save
     * @return nothing, but tosses exceptions if bad things happen
     */
    void Database::createAnimation(const Animation *animation, DatabaseInfo *reply) {

        debug("creating a new animation in the database");

        grpc::Status status;

        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection obtained");

        // Create a BSON doc with this animation
        try {

            // Create the new animationId
            bsoncxx::oid animationId;

            //auto doc_view = animationToBson(animation);
            //trace("doc_value made: {}", bsoncxx::to_json(doc_view));

            //collection.insert_one(doc_view.view());
            trace("run_command done");

            info("saved new animation in the database 💃🏽");
            reply->set_message("✅ Saved new animation in the database");
        }
        catch (const mongocxx::exception &e) {

            // Code 11000 means id collision
            if (e.code().value() == 11000) {
                //std::string errorMessage = fmt::format("attempted to insert a duplicate Animation in the database for id {}", animation->_id());
                //error(errorMessage);
                reply->set_message("Unable to create new animation");
                //reply->set_help(fmt::format("ID {} already exists", animation->_id()));
                //throw creatures::DuplicateFoundError(errorMessage);

            } else {
                std::string errorMessage = fmt::format("Error updating database: {}", e.what());
                critical(errorMessage);
                reply->set_message(
                        fmt::format("Unable to create Animation in database: {} ({})",
                                    e.what(), e.code().value()));
                reply->set_help(e.code().message());
                throw creatures::InternalError(errorMessage);
            }
        }
        catch (creatures::DataFormatException &e) {
            std::string errorMessage = fmt::format("Data format error while creating an animation: {}", e.what());
            error(errorMessage);
            reply->set_message(fmt::format("Unable to create new Animation: {}", e.what()));
            reply->set_help("Sorry! 💜");
            throw creatures::DataFormatException(errorMessage);

        }
        catch (const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while creating an animation: {}", e.what());
            error(errorMessage);
            reply->set_message(fmt::format("Unable to create new animation: {}", e.what()));
            reply->set_help(fmt::format("Check to make sure the message is well-formed"));
            throw creatures::DatabaseError(errorMessage);
        }

    }

}