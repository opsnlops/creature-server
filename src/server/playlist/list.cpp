
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

//    Status CreatureServerImpl::ListPlaylists(grpc::ServerContext *context, const server::PlaylistFilter *request,
//                                             server::ListPlaylistsResponse *response) {
//
//        info("Listing the animations in the database");
//        return db->listPlaylists(request, response);
//    }

    /**
     * List animations in the database for a given creature type
     *
     * @param filter what CreatureType to look for
     * @param playlistsResponse the list to fill out
     * @return the status of this request
     */
//    grpc::Status Database::listPlaylists(const PlaylistFilter *filter, ListPlaylistsResponse *playlistsResponse) {
//
//        trace("attempting to list all of the playlists");
//
//        grpc::Status status;
//
//        uint32_t numberOfPlaylistsFound = 0;
//
//        try {
//            auto collection = getCollection(PLAYLISTS_COLLECTION);
//            trace("collection obtained");
//
//            document query_doc{};
//            document sort_doc{};
//
//            // First pass, sort by name
//            sort_doc << "name" << 1;
//
//            mongocxx::options::find findOptions{};
//            findOptions.sort(sort_doc.view());
//
//            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);
//
//            // Go Mongo, go! 🎉
//            for (auto &&doc: cursor) {
//
//                auto playlist = playlistsResponse->add_playlists();
//
//                Database::bsonToPlaylist(doc, playlist);
//
//                numberOfPlaylistsFound++;
//            }
//        }
//        catch(const DataFormatException &e) {
//            warn("DataFormatException while getting playlists: {}", e.what());
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  fmt::format("🚨 Server-side error while getting playlists: {}", e.what()));
//            return status;
//        }
//        catch(const mongocxx::exception &e) {
//            critical("MongoDB error while attempting to load playlists: {}", e.what());
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  fmt::format("🚨 MongoDB error while attempting to load playlists: {}", e.what()));
//            return status;
//        }
//        catch(const bsoncxx::exception &e) {
//            critical("BSON error while attempting to load playlists: {}", e.what());
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  fmt::format("🚨 BSON error while attempting to load playlists: {}", e.what()));
//            return status;
//        }
//
//        // Return a 404 if nothing as found
//        if(numberOfPlaylistsFound == 0) {
//            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
//                                  "🚫 No playlists for that creature type found");
//            return status;
//        }
//
//        // If we made this far, we're good! 😍
//        status = grpc::Status(grpc::StatusCode::OK,
//                              fmt::format("✅ Found {} playlists", numberOfPlaylistsFound));
//        return status;
//    }

}