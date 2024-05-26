
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;


namespace creatures {

    extern std::shared_ptr<Database> db;

//    Status CreatureServerImpl::GetPlaylist(grpc::ServerContext *context, const server::PlaylistIdentifier *id,
//                                           server::Playlist *playlist) {
//
//        grpc::Status status;
//
//        info("Loading one animation from the database");
//
//        try {
//            db->getPlaylist(id, playlist);
//            status = grpc::Status(grpc::StatusCode::OK,
//                                  "Loaded a playlist from the database",
//                                  fmt::format("Name: {}, Number of Items: {}",
//                                              playlist->name(),
//                                              playlist->items_size()));
//        }
//        catch(const InvalidArgumentException &e) {
//            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
//                                  "PlaylistIdentifier was empty on getPlaylist()",
//                                  fmt::format("⛔️️ A PlaylistIdentifier must be supplied"));
//        }
//        catch(const NotFoundException &e) {
//            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
//                                  fmt::format("⚠️ No playlist with ID '{}' found", bsoncxx::oid(id->_id()).to_string()),
//                                  "Try another ID! 😅");
//        }
//        catch(const DataFormatException &e) {
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  "Unable to encode request into BSON",
//                                  e.what());
//        }
//        catch(const InternalError &e) {
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  fmt::format("MongoDB error while loading a playlist: {}", e.what()),
//                                  e.what());
//        }
//        catch( ... ) {
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  "An unknown error happened while getting a playlist",
//                                  "Default catch hit?");
//        }
//
//        return status;
//    }
//
//
//    void Database::getPlaylist(const PlaylistIdentifier *playlistIdentifier, Playlist *playlist) {
//
//        if (playlistIdentifier->_id().empty()) {
//            error("an empty playlistIdentifier was passed into getPlaylist()");
//            throw InvalidArgumentException("an empty playlistIdentifier was passed into getPlaylist()");
//        }
//
//        auto collection = getCollection(PLAYLISTS_COLLECTION);
//        trace("collection gotten");
//
//
//        try {
//
//            // Convert the ID into an OID
//            trace("attempting to convert the ID");
//            bsoncxx::oid id = bsoncxx::oid(playlistIdentifier->_id().data(), bsoncxx::oid::k_oid_length);
//            debug("found playlistIdentifier ID: {}", id.to_string());
//
//            // Create a filter BSON document to match the target document
//            auto filter = bsoncxx::builder::stream::document{} << "_id" << id << bsoncxx::builder::stream::finalize;
//            trace("filter doc: {}", bsoncxx::to_json(filter));
//
//            // Go try to load it
//            bsoncxx::stdx::optional<bsoncxx::document::value> result = collection.find_one(filter.view());
//
//            if (!result) {
//                info("🚫 No playlist with ID '{}' found", id.to_string());
//                throw NotFoundException(fmt::format("🚫 No playlist with ID '{}' found", id.to_string()));
//            }
//
//            // Get an owning reference to this doc since it's ours now
//            bsoncxx::document::value doc = *result;
//
//            Database::bsonToPlaylist(doc, playlist);
//            debug("loaded the playlist");
//
//
//        }
//        catch (const bsoncxx::exception &e) {
//            error("BSON exception while loading a playlist: {}", e.what());
//            throw DataFormatException(fmt::format("BSON exception while loading a playlist: {}", e.what()));
//        }
//        catch (const mongocxx::exception &e) {
//            error("MongoDB exception while loading a playlist: {}", e.what());
//
//        }
//
//        // Hooray, we loaded it all!
//        info("done loading a playlist");
//
//    }

}