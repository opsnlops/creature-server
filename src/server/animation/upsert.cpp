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
#include "util/helpers.h"

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "model/Track.h"

#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

/**
 * Create or update an animation in the database
 *
 * @param animation the `creatures::Animation` to save
 * @return a status message to return to the client
 */
Result<creatures::Animation> Database::upsertAnimation(const std::string &animationJson,
                                                       std::shared_ptr<OperationSpan> parentSpan) {

    debug("upserting an animation in the database");
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.upsertAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "update_one_upsert");
    }

    try {
        // 🥕 JSON parsing span
        auto parseSpan = creatures::observability->createChildOperationSpan("JSON.parse", dbSpan);
        if (parseSpan)
            parseSpan->setAttribute("input.size_bytes", static_cast<int64_t>(animationJson.length()));
        auto jsonResult = JsonParser::parseJsonString(animationJson, "animation upsert", parseSpan);
        if (!jsonResult.isSuccess()) {
            auto error = jsonResult.getError().value();
            if (dbSpan)
                dbSpan->setError(error.getMessage());
            return Result<creatures::Animation>{error};
        }
        auto jsonObject = jsonResult.getValue().value();

        // ✨ Convert from JSON to our internal Animation object for validation
        auto validationSpan = creatures::observability->createChildOperationSpan("Animation.fromJson", dbSpan);
        auto animationResult = animationFromJson(jsonObject);
        if (!animationResult.isSuccess()) {
            auto error = animationResult.getError();
            std::string errorMessage =
                fmt::format("Error while creating an animation from JSON: {}", error->getMessage());
            warn(errorMessage);
            if (validationSpan)
                validationSpan->setError(errorMessage);
            if (dbSpan)
                dbSpan->setError(errorMessage);
            return Result<creatures::Animation>{ServerError(ServerError::InvalidData, error->getMessage())};
        }
        auto animation = animationResult.getValue().value();
        if (validationSpan) {
            validationSpan->setAttribute("animation.id", animation.id);
            validationSpan->setSuccess();
        }
        if (dbSpan) {
            dbSpan->setAttribute("animation.id", animation.id);
            dbSpan->setAttribute("animation.title", animation.metadata.title);
        }

        // Now go save it in Mongo
        // Create span for getting the MongoDB collection
        auto collectionSpan = creatures::observability->createChildOperationSpan("Database.getCollection", dbSpan);
        auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while upserting an animation: {}", error.getMessage());
            warn(errorMessage);
            if (collectionSpan)
                collectionSpan->setError(errorMessage);
            if (dbSpan)
                dbSpan->setError(errorMessage);
            return Result<creatures::Animation>{error};
        }
        auto collection = collectionResult.getValue().value();
        if (collectionSpan)
            collectionSpan->setSuccess();
        trace("collection obtained");

        // 🌟 BSON conversion span
        auto bsonConversionSpan = creatures::observability->createChildOperationSpan("BSON.fromJson", dbSpan);
        auto bsonResult =
            JsonParser::jsonStringToBson(animationJson, fmt::format("animation {}", animation.id), bsonConversionSpan);
        if (!bsonResult.isSuccess()) {
            auto error = bsonResult.getError().value();
            if (dbSpan)
                dbSpan->setError(error.getMessage());
            return Result<creatures::Animation>{error};
        }
        auto bsonDoc = bsonResult.getValue().value();

        // 🥕 Span for building the query filter
        auto filterSpan = creatures::observability->createChildOperationSpan("Database.buildFilter", dbSpan);
        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << animation.id;
        if (filterSpan) {
            filterSpan->setAttribute("filter.field", "id");
            filterSpan->setAttribute("filter.value", animation.id);
            filterSpan->setSuccess();
        }

        // 🚀 The actual database query
        auto upsertSpan = creatures::observability->createChildOperationSpan("MongoDB.updateOne", dbSpan);
        if (upsertSpan) {
            upsertSpan->setAttribute("db.system", "mongodb");
            upsertSpan->setAttribute("db.name", DB_NAME);
            upsertSpan->setAttribute("db.collection.name", ANIMATIONS_COLLECTION);
            upsertSpan->setAttribute("db.operation", "updateOne");
            upsertSpan->setAttribute("db.options.upsert", true);
            upsertSpan->setAttribute("db.query.filter", fmt::format("{{\"id\": \"{}\"}}", animation.id));
        }

        mongocxx::options::update update_options;
        update_options.upsert(true);

        collection.update_one(filter_builder.view(),
                              bsoncxx::builder::stream::document{} << "$set" << bsonDoc.view()
                                                                   << bsoncxx::builder::stream::finalize,
                              update_options);
        if (upsertSpan)
            upsertSpan->setSuccess();

        info("Animation upserted in the database: {}", animation.id);
        if (dbSpan)
            dbSpan->setSuccess();
        return Result<creatures::Animation>{animation};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (mongocxx::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(errorMessage);
        }
        return Result<creatures::Animation>{ServerError(ServerError::InternalError, e.what())};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage =
            fmt::format("Error (bsoncxx::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(errorMessage);
        }
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, e.what())};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("Error (nlohmann::json::exception) while upserting an animation in database: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(errorMessage);
        }
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, e.what())};
    } catch (...) {
        std::string error_message = "Unknown error while upserting an animation in the database";
        critical(error_message);
        if (dbSpan)
            dbSpan->setError(error_message);
        return Result<creatures::Animation>{ServerError(ServerError::InternalError, error_message)};
    }
}

Result<void> Database::insertAdHocAnimation(const creatures::Animation &animation,
                                            std::chrono::system_clock::time_point createdAt,
                                            std::shared_ptr<OperationSpan> parentSpan) {

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.insertAdHocAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("animation.id", animation.id);
    }

    try {
        auto animationJson = animationToJson(animation);
        auto jsonString = animationJson.dump();

        auto bsonResult =
            JsonParser::jsonStringToBson(jsonString, fmt::format("adhoc animation {}", animation.id), dbSpan);
        if (!bsonResult.isSuccess()) {
            auto error = bsonResult.getError().value();
            if (dbSpan) {
                dbSpan->setError(error.getMessage());
            }
            return Result<void>{error};
        }
        auto bsonDoc = bsonResult.getValue().value();

        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            if (dbSpan) {
                dbSpan->setError(error.getMessage());
            }
            return Result<void>{error};
        }
        auto collection = collectionResult.getValue().value();

        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(createdAt.time_since_epoch());
        auto finalDoc = bsoncxx::builder::stream::document{} << "created_at" << bsoncxx::types::b_date{millis}
                                                             << bsoncxx::builder::stream::concatenate(bsonDoc.view())
                                                             << bsoncxx::builder::stream::finalize;

        collection.insert_one(finalDoc.view());
        info("Inserted ad-hoc animation {} into {}", animation.id, ADHOC_ANIMATIONS_COLLECTION);

        if (dbSpan) {
            dbSpan->setSuccess();
        }

        return Result<void>{};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to insert ad-hoc animation {}: {}", animation.id, e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(errorMessage);
        }
        return Result<void>{ServerError(ServerError::DatabaseError, errorMessage)};
    }
}

Result<std::vector<AdHocAnimationRecord>>
Database::listAdHocAnimations(std::shared_ptr<OperationSpan> parentSpan) {

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listAdHocAnimations", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
    }

    try {
        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            if (dbSpan) {
                dbSpan->setError(collectionResult.getError()->getMessage());
            }
            return Result<std::vector<AdHocAnimationRecord>>{collectionResult.getError().value()};
        }
        auto collection = collectionResult.getValue().value();

        mongocxx::options::find options;
        bsoncxx::builder::stream::document sortDoc;
        sortDoc << "created_at" << -1;
        options.sort(sortDoc.view());

        std::vector<AdHocAnimationRecord> records;
        auto cursor = collection.find({}, options);
        for (const auto &doc : cursor) {
            auto jsonResult = JsonParser::bsonToJson(doc, "adhoc animation list", dbSpan);
            if (!jsonResult.isSuccess()) {
                if (dbSpan) {
                    dbSpan->setError(jsonResult.getError()->getMessage());
                }
                return Result<std::vector<AdHocAnimationRecord>>{jsonResult.getError().value()};
            }

            auto animationResult = animationFromJson(jsonResult.getValue().value());
            if (!animationResult.isSuccess()) {
                if (dbSpan) {
                    dbSpan->setError(animationResult.getError()->getMessage());
                }
                return Result<std::vector<AdHocAnimationRecord>>{animationResult.getError().value()};
            }

            AdHocAnimationRecord record;
            record.animation = animationResult.getValue().value();

            if (doc["created_at"] && doc["created_at"].type() == bsoncxx::type::k_date) {
                auto millis = doc["created_at"].get_date().value;
                record.createdAt =
                    std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
                        millis));
            } else {
                record.createdAt = std::chrono::system_clock::now();
            }

            records.push_back(std::move(record));
        }

        if (dbSpan) {
            dbSpan->setAttribute("count", static_cast<int64_t>(records.size()));
            dbSpan->setSuccess();
        }

        return Result<std::vector<AdHocAnimationRecord>>{records};

    } catch (const std::exception &e) {
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(e.what());
        }
        return Result<std::vector<AdHocAnimationRecord>>{
            ServerError(ServerError::DatabaseError, fmt::format("Failed to list ad-hoc animations: {}", e.what()))};
    }
}

Result<creatures::Animation> Database::getAdHocAnimation(const animationId_t &animationId,
                                                         std::shared_ptr<OperationSpan> parentSpan) {
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAdHocAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ADHOC_ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("animation.id", animationId);
    }

    try {
        auto collectionResult = getCollection(ADHOC_ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            if (dbSpan) {
                dbSpan->setError(collectionResult.getError()->getMessage());
            }
            return Result<creatures::Animation>{collectionResult.getError().value()};
        }
        auto collection = collectionResult.getValue().value();

        auto filterById = bsoncxx::builder::stream::document{} << "id" << animationId
                                                               << bsoncxx::builder::stream::finalize;
        auto docOpt = collection.find_one(filterById.view());
        if (!docOpt) {
            auto filterByMetadataId =
                bsoncxx::builder::stream::document{} << "metadata.animation_id" << animationId
                                                     << bsoncxx::builder::stream::finalize;
            docOpt = collection.find_one(filterByMetadataId.view());
        }
        if (!docOpt) {
            if (dbSpan) {
                dbSpan->setAttribute("result", "not_found");
                dbSpan->setError("Ad-hoc animation not found");
            }
            return Result<creatures::Animation>{
                ServerError(ServerError::NotFound, fmt::format("Ad-hoc animation {} not found", animationId))};
        }

        auto jsonResult = JsonParser::bsonToJson(docOpt->view(), "adhoc animation get", dbSpan);
        if (!jsonResult.isSuccess()) {
            if (dbSpan) {
                dbSpan->setError(jsonResult.getError()->getMessage());
            }
            return Result<creatures::Animation>{jsonResult.getError().value()};
        }

        auto animationResult = animationFromJson(jsonResult.getValue().value());
        if (!animationResult.isSuccess()) {
            if (dbSpan) {
                dbSpan->setError(animationResult.getError()->getMessage());
            }
            return Result<creatures::Animation>{animationResult.getError().value()};
        }

        if (dbSpan) {
            dbSpan->setSuccess();
        }

        return Result<creatures::Animation>{animationResult.getValue().value()};

    } catch (const std::exception &e) {
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(e.what());
        }
        return Result<creatures::Animation>{
            ServerError(ServerError::DatabaseError,
                        fmt::format("Failed to load ad-hoc animation {}: {}", animationId, e.what()))};
    }
}
} // namespace creatures
