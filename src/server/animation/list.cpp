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
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<std::vector<creatures::AnimationMetadata>>
Database::listAnimations(creatures::SortBy sortBy, const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.listAnimations, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.listAnimations", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.sort_by", static_cast<int64_t>(sortBy));
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    debug("attempting to list all of the animations");

    std::vector<creatures::AnimationMetadata> animations;

    try {
        document query_doc{};
        document projection_doc{};
        document sort_doc{};
        sort_doc << "metadata.title" << 1;
        // Don't return the track data — otherwise we'd load most of the
        // collection into memory just to render a list.
        projection_doc << "tracks" << 0;

        mongocxx::options::find findOptions{};
        findOptions.projection(projection_doc.view());
        findOptions.sort(sort_doc.view());

        auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while listing all of the animations: {}", err.getMessage());
            warn(errorMessage);
            setSpanError(errorMessage, "DatabaseError", err.getCode());
            return Result<std::vector<creatures::AnimationMetadata>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("listAnimations.mongoQuery", dbSpan);
        mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

        std::int64_t documentsFailed = 0;
        for (auto &&doc : cursor) {
            auto docSpan =
                creatures::observability->createChildOperationSpan("listAnimations.create-metadata", mongoSpan);
            try {
                auto jsonResult = JsonParser::bsonToJson(doc, "animation document", docSpan);
                if (!jsonResult.isSuccess()) {
                    auto err = jsonResult.getError().value();
                    if (docSpan) {
                        docSpan->setError(err.getMessage());
                        docSpan->setAttribute("error.type", "JsonParsingException");
                        docSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                    }
                    documentsFailed++;
                    continue;
                }
                nlohmann::json json_doc = jsonResult.getValue().value();

                auto metaResult = animationMetadataFromJson(json_doc["metadata"]);
                if (!metaResult.isSuccess()) {
                    auto err = metaResult.getError().value();
                    documentsFailed++;
                    std::string errorMessage =
                        fmt::format("Unable to parse JSON to AnimationMetadata: {}", err.getMessage());
                    warn(errorMessage);
                    if (docSpan) {
                        docSpan->setError(errorMessage);
                        docSpan->setAttribute("error.type", "DataFormatException");
                        docSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                    }
                    continue;
                }

                auto animationMetadata = metaResult.getValue().value();
                animations.push_back(animationMetadata);

                if (docSpan) {
                    docSpan->setAttribute("animation.id", animationMetadata.animation_id);
                    docSpan->setAttribute("animation.title", animationMetadata.title);
                    docSpan->setSuccess();
                }
            } catch (const nlohmann::json::exception &e) {
                documentsFailed++;
                std::string errorMessage = fmt::format("JSON processing error for a document: {}", e.what());
                warn(errorMessage);
                if (docSpan) {
                    docSpan->recordException(e);
                    docSpan->setError(errorMessage);
                    docSpan->setAttribute("error.type", "JsonParsingException");
                    docSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                }
            }
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("animations.count", static_cast<int64_t>(animations.size()));
            mongoSpan->setAttribute("animations.failed", documentsFailed);
            mongoSpan->setSuccess();
        }
    } catch (const DataFormatException &e) {
        std::string errorMessage = fmt::format("Failed to get all animations: {}", e.what());
        warn(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "DataFormatException", ServerError::InvalidData);
        return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("MongoDB Exception while loading animation: {}", e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage = fmt::format("BSON error while attempting to load animations: {}", e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "JsonParsingException", ServerError::InternalError);
        return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    if (animations.empty()) {
        std::string errorMessage = "No animations found";
        warn(errorMessage);
        setSpanError(errorMessage, "NotFound", ServerError::NotFound);
        return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::NotFound, errorMessage)};
    }

    info("done loading {} animations", animations.size());
    if (dbSpan) {
        dbSpan->setAttribute("animations.count", static_cast<int64_t>(animations.size()));
        dbSpan->setSuccess();
    }
    return Result<std::vector<creatures::AnimationMetadata>>{animations};
}

} // namespace creatures
