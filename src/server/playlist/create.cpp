#include "server/config.h"

#include <string>
#include "spdlog/spdlog.h"

#include "server.pb.h"
#include "server/database.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;

    /**
     * Save a new playlist in the database
     *
     * @param context the `ServerContext` of this request
     * @param playlist a `Playlist` to save
     * @param reply a `DatabaseInfo` that will be filled out for the reply
     * @return state of the request
     */
    Status CreatureServerImpl::CreatePlaylist(grpc::ServerContext *context, const server::Playlist *playlist,
                                              server::DatabaseInfo *reply) {

        info("Creating a new playlist in the database");
        return db->createPlaylist(playlist, reply);
    }

    /**
 * Create a new playlist in the database
 *
 * @param playlist the `Playlist` to save
 * @param reply Information about the save
 * @return a gRPC Status on how things went
 */
    grpc::Status Database::createPlaylist(const Playlist *playlist, DatabaseInfo *reply) {

        debug("creating a new playlist in the database");

        grpc::Status status;

        auto collection = getCollection(PLAYLISTS_COLLECTION);
        trace("collection obtained");

        // Create a BSON doc with this playlist
        try {

            // Create the new playlistId
            bsoncxx::oid playlistId;

            auto doc_view = playlistToBson(playlist, playlistId);
            trace("doc_value made: {}", bsoncxx::to_json(doc_view));

            collection.insert_one(doc_view.view());
            trace("run_command done");

            info("saved new playlist in the database 🎶");

            status = grpc::Status(grpc::StatusCode::OK, "✅ Saved new playlist in the database");
            reply->set_message("✅ Saved new playlist in the database");
        }
        catch (const mongocxx::exception &e) {

            // Code 11000 means id collision
            if (e.code().value() == 11000) {
                error("attempted to insert a duplicate Playlist in the database");
                status = grpc::Status(grpc::StatusCode::ALREADY_EXISTS, e.what());
                reply->set_message("Unable to create new playlist");
                reply->set_help("ID already exists");
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
            error("server refused to save playlist: {}", e.what());
            status = grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what());
            reply->set_message(fmt::format("Unable to create new playlist: {}", e.what()));
            reply->set_help("Sorry! 💜");
        }
        catch (const bsoncxx::exception &e) {
            error("unable to convert the incoming playlist to BSON: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
            reply->set_message(fmt::format("Unable to create new playlist: {}", e.what()));
            reply->set_help(fmt::format("Check to make sure the message is well-formed"));
        }

        return status;
    }
}