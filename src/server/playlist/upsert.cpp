
#include "server/config.h"

#include "spdlog/spdlog.h"

// Disable shadow warnings for MongoDB C++ driver headers (third-party code)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>

#pragma GCC diagnostic pop

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<creatures::Playlist> Database::upsertPlaylist(const std::string &playlistJson,
                                                     const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.upsertPlaylist, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertPlaylist", parentSpan);
    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    debug("upserting a playlist in the database");

    try {
        auto jsonSpan = creatures::observability->createChildOperationSpan("upsertPlaylist.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(playlistJson, "playlist upsert", jsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Playlist>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto playlistResult = playlistFromJson(jsonObject, upsertSpan);
        if (!playlistResult.isSuccess()) {
            auto err = playlistResult.getError().value();
            std::string errorMessage = fmt::format("Error while creating a playlist from JSON: {}", err.getMessage());
            warn(errorMessage);
            recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
            return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto playlist = playlistResult.getValue().value();
        if (upsertSpan) {
            upsertSpan->setAttribute("playlist.id", playlist.id);
        }

        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertPlaylist.json-to-bson", upsertSpan);
        auto bsonResult = JsonParser::jsonStringToBson(playlistJson, fmt::format("playlist {}", playlist.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Playlist>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertPlaylist.get-collection", upsertSpan);
        auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("database error upserting a playlist: {}", err.getMessage());
            warn(errorMessage);
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            recordSpanError(upsertSpan, errorMessage, "DatabaseError", err.getCode());
            return Result<creatures::Playlist>{err};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertPlaylist.mongoQuery", upsertSpan);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << playlist.id;

        mongocxx::options::update update_options;
        update_options.upsert(true);

        collection.update_one(filter_builder.view(),
                              bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                                   << bsoncxx::builder::stream::finalize,
                              update_options);
        if (mongoSpan)
            mongoSpan->setSuccess();

        info("Playlist upserted in the database: {}", playlist.id);
        if (upsertSpan) {
            upsertSpan->setAttribute("playlist.name", playlist.name);
            upsertSpan->setAttribute("playlist.number_of_items", static_cast<int64_t>(playlist.number_of_items));
            upsertSpan->setSuccess();
        }
        return Result<creatures::Playlist>{playlist};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting a playlist in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<creatures::Playlist>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting a playlist in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while upserting a playlist in the database";
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<creatures::Playlist>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
