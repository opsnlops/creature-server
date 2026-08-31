
#include "server/config.h"

#include "spdlog/spdlog.h"

#include <optional>

// Disable shadow warnings for MongoDB C++ driver headers (third-party code)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>
#include <mongocxx/options/find.hpp>

#pragma GCC diagnostic pop

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/UuidValidation.h"

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
        upsertSpan->setAttribute("database.operation", "replace_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    debug("upserting a playlist in the database");

    try {
        auto jsonSpan = creatures::observability->createChildOperationSpan("upsertPlaylist.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseApiJsonString(playlistJson, "playlist upsert", jsonSpan);
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
        // Canonicalize the playlist's own identity while retaining the exact
        // lookup spelling of referenced animations. Animation persistence is
        // still case-sensitive for legacy records; the API serializer can emit
        // those references lowercase without silently rewriting storage.
        jsonObject["id"] = playlist.id;
        const auto normalizedJson = jsonObject.dump();
        auto bsonResult =
            JsonParser::jsonStringToBson(normalizedJson, fmt::format("playlist {}", playlist.id), bsonSpan);
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

        auto lookupSpan =
            creatures::observability->createChildOperationSpan("upsertPlaylist.findCanonicalId", upsertSpan);
        if (lookupSpan)
            lookupSpan->setAttribute("database.operation", "find");
        const auto idPattern = fmt::format("^{}$", playlist.id);
        auto compatibilityFilter = bsoncxx::builder::stream::document{} << "id"
                                                                        << bsoncxx::types::b_regex{idPattern, "i"}
                                                                        << bsoncxx::builder::stream::finalize;
        mongocxx::options::find compatibilityOptions;
        compatibilityOptions.limit(2);
        std::optional<std::string> storedId;
        for (const auto &candidate : collection.find(compatibilityFilter.view(), compatibilityOptions)) {
            if (storedId) {
                const auto errorMessage =
                    fmt::format("Multiple playlist records differ only by UUID casing: {}", playlist.id);
                recordSpanError(lookupSpan, errorMessage, "CaseFoldCollision", ServerError::InvalidData);
                recordSpanError(upsertSpan, errorMessage, "CaseFoldCollision", ServerError::InvalidData);
                return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, errorMessage)};
            }
            storedId = std::string(candidate["id"].get_string().value);
        }
        if (lookupSpan) {
            lookupSpan->setAttribute("database.matched_count", static_cast<int64_t>(storedId.has_value()));
            lookupSpan->setSuccess();
        }
        auto filter = bsoncxx::builder::stream::document{} << "id" << storedId.value_or(playlist.id)
                                                           << bsoncxx::builder::stream::finalize;

        // REPLACE, not $set (#135). A $set upsert cannot remove a field, so no
        // caller can ever delete one — the failure is silent and returns 200.
        // See #134, where clearing an accepted voice take did exactly that.
        // The document handed to this function IS the stored document.
        mongocxx::options::replace replace_options;
        replace_options.upsert(true);

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertPlaylist.mongoQuery", upsertSpan);
        if (mongoSpan)
            mongoSpan->setAttribute("database.operation", "replace_one");
        const auto writeResult = collection.replace_one(filter.view(), bsonDoc.view(), replace_options);
        if (mongoSpan) {
            if (writeResult) {
                mongoSpan->setAttribute("database.matched_count", static_cast<int64_t>(writeResult->matched_count()));
                mongoSpan->setAttribute("database.modified_count", static_cast<int64_t>(writeResult->modified_count()));
                mongoSpan->setAttribute("database.upserted", writeResult->upserted_id().has_value());
            }
            mongoSpan->setSuccess();
        }

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
