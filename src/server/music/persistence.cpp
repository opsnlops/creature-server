// Mongo persistence for music pieces (#202). Mirrors src/server/script/ and
// follows docs/database-observability.md: one Database.* span per call with
// database.* attributes, a child span per Mongo round trip, errors recorded
// on the outer span.

#include "server/config.h"

#include <string>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/exception/exception.hpp>

#include "model/MusicPiece.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::finalize;
using json = nlohmann::json;

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

namespace {

void setCollectionAttributes(const std::shared_ptr<OperationSpan> &span, const char *operation) {
    if (span) {
        span->setAttribute("database.collection", MUSIC_PIECES_COLLECTION);
        span->setAttribute("database.operation", operation);
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
    }
}

/// Stored documents carry Mongo's `_id`, which the strict model parser
/// rejects as an unknown field; it is database-owned, not piece data.
Result<MusicPiece> pieceFromStoredJson(json stored) {
    stored.erase("_id");
    return musicPieceFromJson(stored);
}

} // namespace

Result<MusicPiece> Database::getMusicPiece(const std::string &pieceId,
                                           const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getMusicPiece, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getMusicPiece", parentSpan);
    setCollectionAttributes(dbSpan, "find_one");
    if (dbSpan) {
        dbSpan->setAttribute("music.piece_id", pieceId);
    }
    if (pieceId.empty()) {
        const std::string errorMessage = "unable to get a music piece because the id was empty";
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<MusicPiece>{ServerError(ServerError::InvalidData, errorMessage)};
    }
    auto collectionResult = getCollection(MUSIC_PIECES_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
        return Result<MusicPiece>{err};
    }
    auto collectionLease = collectionResult.getValue().value();
    auto &collection = collectionLease->collection();
    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getMusicPiece.mongoQuery", dbSpan);
        auto query = document{} << "id" << pieceId << finalize;
        auto found = collection.find_one(query.view());
        if (mongoSpan)
            mongoSpan->setSuccess();
        if (!found) {
            const std::string errorMessage = fmt::format("Music piece not found: {}", pieceId);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<MusicPiece>{ServerError(ServerError::NotFound, errorMessage)};
        }
        auto convertSpan = creatures::observability->createChildOperationSpan("getMusicPiece.bson-to-json", dbSpan);
        auto jsonResult = JsonParser::bsonToJson(found->view(), fmt::format("music piece {}", pieceId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
            return Result<MusicPiece>{err};
        }
        auto piece = pieceFromStoredJson(jsonResult.getValue().value());
        if (!piece.isSuccess()) {
            auto err = piece.getError().value();
            const std::string errorMessage =
                fmt::format("Stored music piece {} failed validation: {}", pieceId, err.getMessage());
            critical(errorMessage);
            recordSpanError(dbSpan, errorMessage, "DataFormatException", err.getCode());
            return Result<MusicPiece>{ServerError(ServerError::InternalError, errorMessage)};
        }
        if (dbSpan) {
            dbSpan->setAttribute("music.version_count", static_cast<int64_t>(piece.getValue()->versions.size()));
            dbSpan->setSuccess();
        }
        return piece;
    } catch (const mongocxx::exception &e) {
        const std::string errorMessage =
            fmt::format("MongoDB exception caught while finding music piece {}: {}", pieceId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<MusicPiece>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<std::vector<MusicPiece>> Database::listMusicPieces(const std::shared_ptr<OperationSpan> &parentSpan) {
    using ListResult = Result<std::vector<MusicPiece>>;
    if (!parentSpan) {
        warn("no parent span provided for Database.listMusicPieces, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listMusicPieces", parentSpan);
    setCollectionAttributes(dbSpan, "find");
    std::vector<MusicPiece> pieces;
    try {
        auto collectionResult = getCollection(MUSIC_PIECES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return ListResult{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();
        auto mongoSpan = creatures::observability->createChildOperationSpan("listMusicPieces.mongoQuery", dbSpan);
        document sortDoc{};
        sortDoc << "updated_at" << -1; // newest-first, like scripts
        mongocxx::options::find options;
        options.sort(sortDoc.view());
        for (auto doc : collection.find(document{}.view(), options)) {
            auto pieceSpan =
                creatures::observability->createChildOperationSpan("listMusicPieces.create-piece", mongoSpan);
            auto jsonResult = JsonParser::bsonToJson(doc, "music piece document", pieceSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                if (pieceSpan) {
                    pieceSpan->setError(err.getMessage());
                    pieceSpan->setAttribute("error.type", "JsonParsingException");
                }
                continue;
            }
            auto piece = pieceFromStoredJson(jsonResult.getValue().value());
            if (!piece.isSuccess()) {
                // One bad document must not hide the whole library; log it and
                // keep going. getMusicPiece reports it precisely if asked for.
                auto err = piece.getError().value();
                const std::string errorMessage =
                    fmt::format("Skipping stored music piece that failed validation: {}", err.getMessage());
                error(errorMessage);
                if (pieceSpan) {
                    pieceSpan->setError(errorMessage);
                    pieceSpan->setAttribute("error.type", "DataFormatException");
                }
                continue;
            }
            if (pieceSpan) {
                pieceSpan->setAttribute("music.piece_id", piece.getValue()->id);
                pieceSpan->setSuccess();
            }
            pieces.push_back(piece.getValue().value());
        }
        if (mongoSpan)
            mongoSpan->setSuccess();
        if (dbSpan) {
            dbSpan->setAttribute("music.piece_count", static_cast<int64_t>(pieces.size()));
            dbSpan->setSuccess();
        }
        return ListResult{std::move(pieces)};
    } catch (const mongocxx::exception &e) {
        const std::string errorMessage =
            fmt::format("MongoDB exception caught while listing music pieces: {}", e.what());
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return ListResult{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<MusicPiece> Database::upsertMusicPiece(const MusicPiece &piece,
                                              const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.upsertMusicPiece, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertMusicPiece", parentSpan);
    setCollectionAttributes(upsertSpan, "replace_one");
    if (upsertSpan) {
        upsertSpan->setAttribute("music.piece_id", piece.id);
    }
    // Round-trip through the strict parser so nothing the model can't read
    // back is ever stored.
    const auto serialized = musicPieceToJson(piece);
    auto verified = musicPieceFromJson(serialized);
    if (!verified.isSuccess()) {
        auto err = verified.getError().value();
        const std::string errorMessage = fmt::format("Refusing to store an invalid music piece: {}", err.getMessage());
        recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
        return Result<MusicPiece>{ServerError(ServerError::InvalidData, errorMessage)};
    }
    try {
        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertMusicPiece.json-to-bson", upsertSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(serialized.dump(), fmt::format("music piece {}", piece.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<MusicPiece>{err};
        }
        auto collectionResult = getCollection(MUSIC_PIECES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<MusicPiece>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();
        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertMusicPiece.mongoQuery", upsertSpan);
        document filter{};
        filter << "id" << piece.id;
        // REPLACE, not $set (#134): what is handed here IS the stored document.
        mongocxx::options::replace options;
        options.upsert(true);
        collection.replace_one(filter.view(), bsonResult.getValue().value().view(), options);
        if (mongoSpan)
            mongoSpan->setSuccess();
        if (upsertSpan) {
            upsertSpan->setAttribute("music.title", piece.title);
            upsertSpan->setAttribute("music.version_count", static_cast<int64_t>(piece.versions.size()));
            upsertSpan->setSuccess();
        }
        info("Music piece upserted in the database: {}", piece.id);
        return verified;
    } catch (const mongocxx::exception &e) {
        const std::string errorMessage =
            fmt::format("MongoDB exception caught while upserting music piece {}: {}", piece.id, e.what());
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<MusicPiece>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteMusicPiece(const std::string &pieceId, const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.deleteMusicPiece, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.deleteMusicPiece", parentSpan);
    setCollectionAttributes(span, "delete_one");
    if (span) {
        span->setAttribute("music.piece_id", pieceId);
    }
    if (pieceId.empty()) {
        const std::string errorMessage = "deleteMusicPiece called with an empty id";
        recordSpanError(span, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }
    try {
        auto collectionResult = getCollection(MUSIC_PIECES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();
        auto mongoSpan = creatures::observability->createChildOperationSpan("deleteMusicPiece.mongoQuery", span);
        document filter{};
        filter << "id" << pieceId;
        auto result = collection.delete_one(filter.view());
        if (mongoSpan)
            mongoSpan->setSuccess();
        if (!result || result->deleted_count() == 0) {
            const std::string errorMessage = fmt::format("Music piece {} not found while deleting", pieceId);
            recordSpanError(span, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }
        if (span)
            span->setSuccess();
        return Result<void>{};
    } catch (const mongocxx::exception &e) {
        const std::string errorMessage = fmt::format("Error while deleting music piece {}: {}", pieceId, e.what());
        error(errorMessage);
        recordSpanError(span, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
