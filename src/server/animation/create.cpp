
#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>


#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/animation/animation.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

extern creatures::Database *db;

namespace creatures {



    /**
     * Save a new animation in the database
     *
     * @param context the `ServerContext` of this request
     * @param animation an `Animation` to save
     * @param reply a `DatabaseInfo` that will be filled out for the reply
     * @return state of the request
     */
    Status CreatureServerImpl::CreateAnimation(ServerContext *context,
                                               const Animation *animation,
                                               DatabaseInfo *reply) {

        info("Creating a new animation in the database");
        return db->createAnimation(animation, reply);
    }

    /**
     * Create a new animation in the database
     *
     * @param animation the `Animation` to save
     * @param reply Information about the save
     * @return a gRPC Status on how things went
     */
    grpc::Status Database::createAnimation(const Animation *animation, DatabaseInfo *reply) {

        debug("creating a new animation in the database");

        grpc::Status status;

        auto collection = getCollection(ANIMATIONS_COLLECTION);
        trace("collection obtained");

        // Create a BSON doc with this animation
        try {
            auto doc_view = animationToBson(animation, true);
            trace("doc_value made: {}", bsoncxx::to_json(doc_view));

            collection.insert_one(doc_view.view());
            trace("run_command done");

            info("saved new animation in the database 💃🏽");

            status = grpc::Status(grpc::StatusCode::OK, "✅ Saved new animation in the database");
            reply->set_message("✅ Saved new animation in the database");
        }
        catch (const mongocxx::exception &e) {

            // Code 11000 means id collision
            if (e.code().value() == 11000) {
                error("attempted to insert a duplicate Animation in the database for id {}", animation->_id());
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new animation");
                reply->set_help(fmt::format("ID {} already exists", animation->_id()));
            } else {
                critical("Error updating database: {}", e.what());
                status = grpc::Status(grpc::StatusCode::UNKNOWN, e.what(), fmt::to_string(e.code().value()));
                reply->set_message(
                        fmt::format("Unable to create Animation in database: {} ({})",
                                    e.what(), e.code().value()));
                reply->set_help(e.code().message());
            }
        }
        catch (creatures::DataFormatException &e) {
            error("server refused to save animation: {}", e.what());
            status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
            reply->set_message(fmt::format("Unable to create new Animation: {}", e.what()));
            reply->set_help("Sorry! 💜");
        }
        catch (const bsoncxx::exception &e) {
            error("unable to convert the incoming animation to BSON: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
            reply->set_message(fmt::format("Unable to create new animation: {}", e.what()));
            reply->set_help(fmt::format("Check to make sure the message is well-formed"));
        }

        return status;
    }




}