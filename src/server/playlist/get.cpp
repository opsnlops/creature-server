
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
using bsoncxx::builder::stream::finalize;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<json> Database::getPlaylistJson(const playlistId_t &playlistId,
                                       const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getPlaylistJson, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getPlaylistJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("playlist.id", playlistId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    debug("attempting to get the JSON for a playlist by ID: {}", playlistId);

    if (playlistId.empty()) {
        std::string errorMessage = "unable to get a playlist because the id was empty";
        info(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("database error while getting a playlist: {}", err.getMessage());
        warn(errorMessage);
        setSpanError(errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getPlaylistJson.mongoQuery", dbSpan);

        auto filter = document{} << "id" << playlistId << finalize;
        auto maybe_result = collection.find_one(filter.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("Playlist not found: {}", playlistId);
            warn(errorMessage);
            setSpanError(errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getPlaylistJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("playlist {}", playlistId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for playlist {}: {}", playlistId, err.getMessage());
            setSpanError(err.getMessage(), "JsonParsingException", err.getCode());
            return jsonResult;
        }
        nlohmann::json j = jsonResult.getValue().value();

        if (dbSpan) {
            dbSpan->setAttribute("db.response_size_bytes", static_cast<int64_t>(j.dump().length()));
            dbSpan->setSuccess();
        }
        return Result<json>{j};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("MongoDB error while loading playlist {}: {}", playlistId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Error while loading playlist {}: {}", playlistId, e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while loading playlist {}", playlistId);
        critical(errorMessage);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::Playlist> Database::getPlaylist(const playlistId_t &playlistId,
                                                  const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getPlaylist, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getPlaylist", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("playlist.id", playlistId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (playlistId.empty()) {
        std::string errorMessage = "unable to get a playlist because the id was empty";
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getPlaylist.getPlaylistJson", dbSpan);
    auto playlistJson = getPlaylistJson(playlistId, jsonSpan);
    if (!playlistJson.isSuccess()) {
        auto err = playlistJson.getError().value();
        std::string errorMessage = fmt::format("unable to get a playlist by ID: {}", err.getMessage());
        warn(errorMessage);
        std::string etype = "InternalError";
        if (err.getCode() == ServerError::NotFound)
            etype = "NotFound";
        else if (err.getCode() == ServerError::InvalidData)
            etype = "InvalidData";
        else if (err.getCode() == ServerError::DatabaseError)
            etype = "DatabaseError";
        if (jsonSpan) {
            jsonSpan->setError(errorMessage);
            jsonSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        setSpanError(errorMessage, etype, err.getCode());
        return Result<creatures::Playlist>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getPlaylist.playlistFromJson", dbSpan);
    auto result = playlistFromJson(playlistJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a playlist by ID: {}", err.getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        setSpanError(errorMessage, "InvalidData", err.getCode());
        return Result<creatures::Playlist>{err};
    }
    if (fetchSpan)
        fetchSpan->setSuccess();

    auto playlist = result.getValue().value();
    if (dbSpan) {
        dbSpan->setAttribute("playlist.name", playlist.name);
        dbSpan->setAttribute("playlist.number_of_items", static_cast<int64_t>(playlist.number_of_items));
        dbSpan->setSuccess();
    }
    return Result<creatures::Playlist>{playlist};
}

} // namespace creatures
