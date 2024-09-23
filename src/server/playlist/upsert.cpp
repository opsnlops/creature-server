
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>


#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;

    Result<creatures::Playlist> Database::upsertPlaylist(const std::string& playlistJson) {

        debug("upserting a playlist in the database");

        try {
            auto jsonObject = nlohmann::json::parse(playlistJson);

            auto playlistResult = playlistFromJson(jsonObject);
            if (!playlistResult.isSuccess()) {
                auto error = playlistResult.getError();
                warn("Error while creating a playlist from JSON: {}", error->getMessage());
                return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, error->getMessage())};
            }
            auto playlist = playlistResult.getValue().value();

            // Now go save it in Mongo
            auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error upserting a playlist: {}", error.getMessage());
                warn(errorMessage);
                return Result<creatures::Playlist>{error};
            }
            auto collection = collectionResult.getValue().value();
            trace("collection obtained");

            // Convert the JSON string into BSON
            auto bsonDoc = bsoncxx::from_json(playlistJson);

            bsoncxx::builder::stream::document filter_builder;
            filter_builder << "id" << playlist.id;

            // Upsert options
            mongocxx::options::update update_options;
            update_options.upsert(true);

            // Upsert operation
            collection.update_one(
                    filter_builder.view(),
                    bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                         << bsoncxx::builder::stream::finalize,
                    update_options
            );

            info("Playlist upserted in the database: {}", playlist.id);
            return Result<creatures::Playlist>{playlist};

        } catch (const mongocxx::exception &e) {
            error("Error (mongocxx::exception) while upserting a playlist in database: {}", e.what());
            return Result<creatures::Playlist>{ServerError(ServerError::InternalError, e.what())};
        } catch (const bsoncxx::exception &e) {
            error("Error (bsoncxx::exception) while upserting a playlist in database: {}", e.what());
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, e.what())};
        } catch (...) {
            std::string error_message = "Unknown error while upserting a playlist in the database";
            critical(error_message);
            return Result<creatures::Playlist>{ServerError(ServerError::InternalError, error_message)};
        }
    }

}