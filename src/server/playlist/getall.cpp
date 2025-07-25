
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

Result<std::vector<creatures::Playlist>> Database::getAllPlaylists(std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        parentSpan = observability->createOperationSpan("get_all_playlists");
    }
    auto span = observability->createChildOperationSpan("playlists.get_all_playlists", parentSpan);

    debug("attempting to get all of the playlists");

    std::vector<creatures::Playlist> playlists;

    try {
        auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while getting all of the playlists: {}", error.getMessage());
            warn(errorMessage);
            span->setError(errorMessage);
            return Result<std::vector<creatures::Playlist>>{error};
        }

        auto dbSpan = observability->createChildOperationSpan("playlists.get_all_playlists_db", span);
        auto collection = collectionResult.getValue().value();
        trace("collection obtained");

        document query_doc{};
        document projection_doc{};
        document sort_doc{};

        // Only sort by name
        sort_doc << "name" << 1;

        mongocxx::options::find findOptions{};
        findOptions.projection(projection_doc.view());
        findOptions.sort(sort_doc.view());

        mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

        // Go Mongo, go! 🎉
        for (auto &&doc : cursor) {

            std::string json_str = bsoncxx::to_json(doc);
            debug("Playlist JSON out of Mongo: {}", json_str);

            // Parse JSON string to nlohmann::json
            nlohmann::json json_doc = nlohmann::json::parse(json_str);

            // Create the playlist from JSON
            auto playlistResult = playlistFromJson(json_doc, dbSpan);
            if (!playlistResult.isSuccess()) {
                std::string errorMessage = fmt::format("Unable to parse the JSON in the database to Playlist: {}",
                                                       playlistResult.getError()->getMessage());
                warn(errorMessage);
                dbSpan->setError(errorMessage);
                return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            auto playlist = playlistResult.getValue().value();
            playlists.push_back(playlist);
            debug("found {}", playlist.name);
        }
        dbSpan->setAttribute("playlists_found", static_cast<uint64_t>(playlists.size()));
        dbSpan->setSuccess();
    } catch (const DataFormatException &e) {
        std::string errorMessage = fmt::format("Failed to get all playlists: {}", e.what());
        warn(errorMessage);
        span->recordException(e);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("MongoDB Exception while loading the playlists: {}", e.what());
        critical(errorMessage);
        span->recordException(e);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage = fmt::format("BSON error while attempting to load all the playlists: {}", e.what());
        critical(errorMessage);
        span->recordException(e);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    // Return a 404 if nothing as found
    if (playlists.empty()) {
        std::string errorMessage = fmt::format("No playlists found");
        warn(errorMessage);
        span->setError(errorMessage);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::NotFound, errorMessage)};
    }

    info("done loading all the playlists");
    return Result<std::vector<creatures::Playlist>>{playlists};
}

} // namespace creatures