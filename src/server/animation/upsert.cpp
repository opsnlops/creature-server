#include "server/config.h"

#include "spdlog/spdlog.h"
#include <chrono>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/builder/stream/helpers.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/helpers.h"

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Track.h"

#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

using bsoncxx::builder::stream::document;

// Conforms to docs/database-observability.md (issue #17). The previous
// implementations sprouted many tiny non-standard sub-spans (`JSON.parse`,
// `BSON.fromJson`, `MongoDB.updateOne`, `Database.buildFilter`,
// `Database.deleteAnimation.cleanupTracks`, etc.) which added trace noise
// without aiding filtering. Standard names + the one mongoQuery wrapper
// give us everything queryable in Honeycomb.

Result<creatures::Animation> Database::upsertAnimation(const std::string &animationJson,
                                                       const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.upsertAnimation, creating a root span");
    }
    auto upsertSpan = creatures::observability->createChildOperationSpan("Database.upsertAnimation", parentSpan);
    if (upsertSpan) {
        upsertSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        upsertSpan->setAttribute("database.operation", "update_one");
        upsertSpan->setAttribute("database.system", "mongodb");
        upsertSpan->setAttribute("database.name", DB_NAME);
    }

    debug("upserting an animation in the database");
    try {
        auto parseJsonSpan =
            creatures::observability->createChildOperationSpan("upsertAnimation.parse-json", upsertSpan);
        auto jsonResult = JsonParser::parseJsonString(animationJson, "animation upsert", parseJsonSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Animation>{err};
        }
        auto jsonObject = jsonResult.getValue().value();

        auto validateSpan =
            creatures::observability->createChildOperationSpan("upsertAnimation.animationFromJson", upsertSpan);
        auto animationResult = animationFromJson(jsonObject);
        if (!animationResult.isSuccess()) {
            auto err = animationResult.getError().value();
            std::string errorMessage = fmt::format("Error while creating an animation from JSON: {}", err.getMessage());
            warn(errorMessage);
            if (validateSpan) {
                validateSpan->setError(errorMessage);
                validateSpan->setAttribute("error.type", "InvalidData");
                validateSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            recordSpanError(upsertSpan, errorMessage, "InvalidData", err.getCode());
            return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        auto animation = animationResult.getValue().value();
        if (validateSpan)
            validateSpan->setSuccess();
        if (upsertSpan)
            upsertSpan->setAttribute("animation.id", animation.id);

        auto bsonSpan = creatures::observability->createChildOperationSpan("upsertAnimation.json-to-bson", upsertSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(animationJson, fmt::format("animation {}", animation.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(upsertSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Animation>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionSpan =
            creatures::observability->createChildOperationSpan("upsertAnimation.get-collection", upsertSpan);
        auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("database error while upserting an animation: {}", err.getMessage());
            warn(errorMessage);
            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseError");
                collectionSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            recordSpanError(upsertSpan, errorMessage, "DatabaseError", err.getCode());
            return Result<creatures::Animation>{err};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();

        auto mongoSpan = creatures::observability->createChildOperationSpan("upsertAnimation.mongoQuery", upsertSpan);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << animation.id;

        mongocxx::options::update update_options;
        update_options.upsert(true);

        collection.update_one(filter_builder.view(),
                              bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                                   << bsoncxx::builder::stream::finalize,
                              update_options);
        if (mongoSpan)
            mongoSpan->setSuccess();

        info("Animation upserted in the database: {}", animation.id);
        if (upsertSpan) {
            upsertSpan->setAttribute("animation.title", animation.metadata.title);
            upsertSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
            upsertSpan->setAttribute("animation.number_of_frames",
                                     static_cast<int64_t>(animation.metadata.number_of_frames));
            upsertSpan->setSuccess();
        }
        return Result<creatures::Animation>{animation};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<creatures::Animation>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("Error (nlohmann::json::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (upsertSpan)
            upsertSpan->recordException(e);
        recordSpanError(upsertSpan, errorMessage, "JsonParsingException", ServerError::InvalidData);
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Unknown error while upserting an animation in the database";
        critical(errorMessage);
        recordSpanError(upsertSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<creatures::Animation>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::deleteAnimation(const animationId_t &animationId,
                                       const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.deleteAnimation, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.deleteAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "delete_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.id", animationId);
    }

    debug("request received to delete animation {}", animationId);

    if (animationId.empty()) {
        std::string errorMessage = "Animation id cannot be empty when deleting";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    // Load first to confirm existence + capture track count for the span. The
    // mongo delete_one below would also detect not-found, but loading lets us
    // attribute richer success metadata.
    auto loadSpan = creatures::observability->createChildOperationSpan("deleteAnimation.load", dbSpan);
    auto animationResult = getAnimation(animationId, loadSpan);
    if (!animationResult.isSuccess()) {
        auto err = animationResult.getError().value();
        std::string etype = (err.getCode() == ServerError::NotFound) ? "NotFound" : "InvalidData";
        recordSpanError(dbSpan, err.getMessage(), etype, err.getCode());
        return Result<void>{err};
    }
    auto animation = animationResult.getValue().value();
    if (loadSpan) {
        loadSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        loadSpan->setSuccess();
    }

    auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        warn("Failed to get animations collection while deleting {}: {}", animationId, err.getMessage());
        recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
        return Result<void>{err};
    }
    auto collection = collectionResult.getValue().value();

    try {
        auto mongoSpan = creatures::observability->createChildOperationSpan("deleteAnimation.mongoQuery", dbSpan);
        document filter_builder;
        filter_builder << "id" << animationId;

        auto result = collection.delete_one(filter_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!result || result->deleted_count() == 0) {
            std::string errorMessage = fmt::format("Animation {} not found while attempting delete", animationId);
            warn(errorMessage);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<void>{ServerError(ServerError::NotFound, errorMessage)};
        }

        info("Animation {} deleted ({} tracks removed)", animationId, animation.tracks.size());
        if (dbSpan) {
            dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
            dbSpan->setSuccess();
        }
        return Result<void>{};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("MongoDB exception while deleting animation {}: {}", animationId, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage =
            fmt::format("Unexpected error while deleting animation {}: {}", animationId, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while deleting animation {}", animationId);
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<void> Database::insertAdHocAnimation(const creatures::Animation &animation,
                                            std::chrono::system_clock::time_point createdAt,
                                            std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.insertAdHocAnimation, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.insertAdHocAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "insert_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.id", animation.id);
    }

    try {
        auto animationJson = animationToJson(animation);
        auto jsonString = animationJson.dump();

        auto bsonSpan = creatures::observability->createChildOperationSpan("insertAdHocAnimation.json-to-bson", dbSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(jsonString, fmt::format("adhoc animation {}", animation.id), bsonSpan);
        if (!bsonResult.isSuccess()) {
            auto err = bsonResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<void>{err};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("insertAdHocAnimation.mongoQuery", dbSpan);
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(createdAt.time_since_epoch());
        auto finalDoc = bsoncxx::builder::stream::document{} << "created_at" << bsoncxx::types::b_date{millis}
                                                             << bsoncxx::builder::stream::concatenate(bsonDoc.view())
                                                             << bsoncxx::builder::stream::finalize;

        collection.insert_one(finalDoc.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        info("Inserted ad-hoc animation {} into {}", animation.id, ADHOC_ANIMATIONS_COLLECTION);

        if (dbSpan) {
            dbSpan->setAttribute("animation.title", animation.metadata.title);
            dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
            dbSpan->setSuccess();
        }

        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to insert ad-hoc animation {}: {}", animation.id, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<std::vector<AdHocAnimationRecord>> Database::listAdHocAnimations(std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listAdHocAnimations, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listAdHocAnimations", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    try {
        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<std::vector<AdHocAnimationRecord>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listAdHocAnimations.mongoQuery", dbSpan);
        mongocxx::options::find options;
        bsoncxx::builder::stream::document sortDoc;
        sortDoc << "created_at" << -1;
        options.sort(sortDoc.view());

        std::vector<AdHocAnimationRecord> records;
        auto cursor = collection.find({}, options);
        for (const auto &doc : cursor) {
            auto jsonResult = JsonParser::bsonToJson(doc, "adhoc animation list", mongoSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
                return Result<std::vector<AdHocAnimationRecord>>{err};
            }

            auto animationResult = animationFromJson(jsonResult.getValue().value());
            if (!animationResult.isSuccess()) {
                auto err = animationResult.getError().value();
                recordSpanError(dbSpan, err.getMessage(), "DataFormatException", err.getCode());
                return Result<std::vector<AdHocAnimationRecord>>{err};
            }

            AdHocAnimationRecord record;
            record.animation = animationResult.getValue().value();

            if (doc["created_at"] && doc["created_at"].type() == bsoncxx::type::k_date) {
                auto millis = doc["created_at"].get_date().value;
                record.createdAt = std::chrono::system_clock::time_point(
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(millis));
            } else {
                record.createdAt = std::chrono::system_clock::now();
            }

            records.push_back(std::move(record));
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("adhoc_animations.count", static_cast<int64_t>(records.size()));
            mongoSpan->setSuccess();
        }

        if (dbSpan) {
            dbSpan->setAttribute("adhoc_animations.count", static_cast<int64_t>(records.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<AdHocAnimationRecord>>{records};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list ad-hoc animations: {}", e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<std::vector<AdHocAnimationRecord>>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<creatures::Animation> Database::getAdHocAnimation(const animationId_t &animationId,
                                                         std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAdHocAnimation, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAdHocAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.id", animationId);
    }

    try {
        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
            return Result<creatures::Animation>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("getAdHocAnimation.mongoQuery", dbSpan);
        auto filterById = bsoncxx::builder::stream::document{} << "id" << animationId
                                                               << bsoncxx::builder::stream::finalize;
        auto docOpt = collection.find_one(filterById.view());
        if (!docOpt) {
            // Fallback: older docs stored the id under metadata.animation_id only.
            auto filterByMetadataId = bsoncxx::builder::stream::document{} << "metadata.animation_id" << animationId
                                                                           << bsoncxx::builder::stream::finalize;
            docOpt = collection.find_one(filterByMetadataId.view());
        }
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!docOpt) {
            std::string errorMessage = fmt::format("Ad-hoc animation {} not found", animationId);
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<creatures::Animation>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getAdHocAnimation.bson-to-json", dbSpan);
        auto jsonResult = JsonParser::bsonToJson(docOpt->view(), "adhoc animation get", convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
            return Result<creatures::Animation>{err};
        }

        auto animationResult = animationFromJson(jsonResult.getValue().value());
        if (!animationResult.isSuccess()) {
            auto err = animationResult.getError().value();
            recordSpanError(dbSpan, err.getMessage(), "InvalidData", err.getCode());
            return Result<creatures::Animation>{err};
        }

        auto animation = animationResult.getValue().value();
        if (dbSpan) {
            dbSpan->setAttribute("animation.title", animation.metadata.title);
            dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
            dbSpan->setSuccess();
        }

        return Result<creatures::Animation>{animation};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to load ad-hoc animation {}: {}", animationId, e.what());
        error(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::DatabaseError);
        return Result<creatures::Animation>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

} // namespace creatures
