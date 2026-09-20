
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <optional>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/options/find.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"

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
    const auto canonicalId = isUuidShape(playlistId) ? canonicalUuid(playlistId) : playlistId;

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("playlist.id", canonicalId);
    }

    debug("attempting to get the JSON for a playlist by ID: {}", playlistId);

    if (!isUuidShape(playlistId)) {
        std::string errorMessage = "unable to get a playlist because the id was not a UUID";
        info(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage = fmt::format("database error while getting a playlist: {}", err.getMessage());
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collectionLease = collectionResult.getValue().value();
    auto &collection = collectionLease->collection();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getPlaylistJson.mongoQuery", dbSpan);

        const auto idPattern = fmt::format("^{}$", canonicalId);
        auto filter = document{} << "id" << bsoncxx::types::b_regex{idPattern, "i"} << finalize;
        mongocxx::options::find options;
        mongo::applyOperationDeadline(options);
        options.limit(2);
        auto cursor = collection.find(filter.view(), options);
        std::optional<bsoncxx::document::value> maybeResult;
        for (const auto &candidate : cursor) {
            if (maybeResult) {
                const auto errorMessage =
                    fmt::format("Multiple playlist records differ only by UUID casing: {}", canonicalId);
                recordSpanError(mongoSpan, errorMessage, "CaseFoldCollision", ServerError::InvalidData);
                recordSpanError(dbSpan, errorMessage, "CaseFoldCollision", ServerError::InvalidData);
                return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            maybeResult.emplace(candidate);
        }
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybeResult) {
            std::string errorMessage = fmt::format("Playlist not found: {}", playlistId);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getPlaylistJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybeResult->view(), fmt::format("playlist {}", playlistId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for playlist {}: {}", playlistId, err.getMessage());
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
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
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Error while loading playlist {}: {}", playlistId, e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while loading playlist {}", playlistId);
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::Playlist> Database::getPlaylist(const playlistId_t &playlistId,
                                                  const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getPlaylist, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getPlaylist", parentSpan);
    const auto canonicalId = isUuidShape(playlistId) ? canonicalUuid(playlistId) : playlistId;

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("playlist.id", canonicalId);
    }

    if (!isUuidShape(playlistId)) {
        std::string errorMessage = "unable to get a playlist because the id was not a UUID";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getPlaylist.getPlaylistJson", dbSpan);
    auto playlistJson = getPlaylistJson(canonicalId, jsonSpan);
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
        recordSpanError(dbSpan, errorMessage, etype, err.getCode());
        return Result<creatures::Playlist>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getPlaylist.playlistFromJson", dbSpan);
    auto result = playlistFromStoredJson(playlistJson.getValue().value(), fetchSpan);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get a playlist by ID: {}", err.getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        recordSpanError(dbSpan, errorMessage, "InvalidData", err.getCode());
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
