
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

Result<json> Database::getPlaylistJson(playlistId_t playlistId, std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        parentSpan = observability->createOperationSpan("playlist.without_parent");
    }
    auto span = observability->createChildOperationSpan("playlist.get_playlist_json", parentSpan);
    span->setAttribute("playlist_id", playlistId);

    debug("attempting to get the JSON for a playlist by ID: {}", playlistId);

    if (playlistId.empty()) {
        std::string errorMessage = fmt::format("an empty playlistId was passed into getPlaylistJson()");
        info(errorMessage);
        span->setError(errorMessage);
        return Result<json>{ServerError(ServerError::InvalidData, "unable to get a playlist because the id was empty")};
    }

    auto dbSpan = observability->createChildOperationSpan("playlist.get_playlist_json.database");
    try {
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << playlistId;

        // Search for the document
        auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            std::string errorMessage = fmt::format("database error while getting a playlist: {}", error.getMessage());
            warn(errorMessage);
            dbSpan->setError(errorMessage);
            return Result<json>{error};
        }
        auto collection = collectionResult.getValue().value();
        auto maybe_result = collection.find_one(filter_builder.view());

        // Check if the document was found
        if (maybe_result) {
            // Convert BSON document to JSON using utility
            bsoncxx::document::view view = maybe_result->view();

            auto jsonResult = JsonParser::bsonToJson(view, fmt::format("playlist {}", playlistId), dbSpan);
            if (!jsonResult.isSuccess()) {
                auto error = jsonResult.getError().value();
                span->setError(error.getMessage());
                return jsonResult;
            }
            nlohmann::json json_result = jsonResult.getValue().value();

            dbSpan->setSuccess();
            span->setSuccess();
            return Result<json>{json_result};
        } else {
            std::string errorMessage = fmt::format("no playlist id '{}' found", playlistId);
            warn(errorMessage);
            dbSpan->setError(errorMessage);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }
    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("a MongoDB error happened while loading a playlist by ID: {}", e.what());
        critical(errorMessage);
        dbSpan->recordException(e);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("An unknown error happened while loading a playlist by ID");
        critical(errorMessage);
        dbSpan->setError(errorMessage);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::Playlist> Database::getPlaylist(const playlistId_t &playlistId,
                                                  std::shared_ptr<OperationSpan> parentSpan) {

    if (!parentSpan) {
        parentSpan = observability->createOperationSpan("playlist.without_parent");
    }
    auto span = observability->createChildOperationSpan("playlist.get_playlist", parentSpan);
    span->setAttribute("playlist_id", playlistId);

    if (playlistId.empty()) {
        std::string errorMessage = "unable to get a playlist because the id was empty";
        warn(errorMessage);
        span->setError(errorMessage);
        return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    // Go to the database and get the playlist's raw JSON
    auto playlistJson = getPlaylistJson(playlistId, span);
    if (!playlistJson.isSuccess()) {
        auto error = playlistJson.getError().value();
        std::string errorMessage =
            fmt::format("unable to get a playlist by ID: {}", playlistJson.getError()->getMessage());
        warn(errorMessage);
        span->setError(errorMessage);
        return Result<creatures::Playlist>{error};
    }

    // Covert it to an Playlist object (if possible)
    auto result = playlistFromJson(playlistJson.getValue().value(), span);
    if (!result.isSuccess()) {
        auto error = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a playlist by ID: {}", result.getError()->getMessage());
        warn(errorMessage);
        span->setError(errorMessage);
        return Result<creatures::Playlist>{error};
    }

    // Create the playlist
    auto playlist = result.getValue().value();
    span->setAttribute("playlist_name", playlist.name);
    span->setAttribute("playlist_number_of_items", playlist.number_of_items);
    span->setSuccess();
    return Result<creatures::Playlist>{playlist};
}
} // namespace creatures