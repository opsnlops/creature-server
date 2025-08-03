#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
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
        auto jsonObject = nlohmann::json::parse(animationJson);
        if (parseSpan)
            parseSpan->setSuccess();

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
        auto bsonDoc = bsoncxx::from_json(animationJson);
        if (bsonConversionSpan) {
            bsonConversionSpan->setAttribute("json.size_bytes", static_cast<int64_t>(animationJson.length()));
            bsonConversionSpan->setAttribute("bson.size_bytes", static_cast<int64_t>(bsonDoc.view().length()));
            bsonConversionSpan->setSuccess();
        }

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
} // namespace creatures