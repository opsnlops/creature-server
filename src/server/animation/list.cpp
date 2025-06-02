#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>


#include "server/database.h"
#include "exception/exception.h"
#include "server/creature-server.h"
#include "util/helpers.h"
#include "util/ObservabilityManager.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObservabilityManager> observability;


    /**
     * List animations in the database for a given creature type
     *
     * @param sortBy How to sort the list (currently unused)
     * @param parentSpan The parent span for tracing, if any
     * @return the status of this request
     */
    Result<std::vector<creatures::AnimationMetadata>> Database::listAnimations(creatures::SortBy sortBy, const std::shared_ptr<OperationSpan>& parentSpan) {

        debug("attempting to list all of the animations");
        auto dbSpan = creatures::observability->createChildOperationSpan("Database.listAnimations",
                                                                         parentSpan);

        if (dbSpan) {
            dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
            dbSpan->setAttribute("database.operation", "find");
            dbSpan->setAttribute("animation.sort_by", static_cast<int64_t>(sortBy));
        }

        std::vector<creatures::AnimationMetadata> animations;

        try {
            document query_doc{};
            document projection_doc{};
            document sort_doc{};

            // We only want to sort by name at this point
            sort_doc << "metadata.title" << 1;

            // Don't return the track data. Otherwise we'd be loading most of the
            // entire collection into memory just to get a list!
            projection_doc << "tracks" << 0;

            mongocxx::options::find findOptions{};
            findOptions.projection(projection_doc.view());
            findOptions.sort(sort_doc.view());

            // 🐰 Span for getting the MongoDB collection
            auto collectionSpan = creatures::observability->createChildOperationSpan("Database.getCollection", dbSpan);
            if (collectionSpan) {
                collectionSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
                collectionSpan->setAttribute("operation", "get_collection");
            }

            auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
            if(!collectionResult.isSuccess()) {
                auto error = collectionResult.getError().value();
                std::string errorMessage = fmt::format("database error while listing all of the animations: {}", error.getMessage());
                warn(errorMessage);

                if(collectionSpan) {
                    collectionSpan->setError(errorMessage);
                }
                if (dbSpan) {
                    dbSpan->setError(errorMessage);
                    dbSpan->setAttribute("error.type", "DatabaseError");
                    dbSpan->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
                }

                return Result<std::vector<creatures::AnimationMetadata>>{error};
            }
            auto collection = collectionResult.getValue().value();
            if(collectionSpan) {
                collectionSpan->setSuccess();
            }

            // 🌟 The main MongoDB query span
            auto querySpan = creatures::observability->createChildOperationSpan("MongoDB.find", dbSpan);
            if (querySpan) {
                querySpan->setAttribute("db.system", "mongodb");
                querySpan->setAttribute("db.name", DB_NAME);
                querySpan->setAttribute("db.collection.name", ANIMATIONS_COLLECTION);
                querySpan->setAttribute("db.operation", "find");
                querySpan->setAttribute("db.query.projection", bsoncxx::to_json(projection_doc.view()));
                querySpan->setAttribute("db.query.sort", bsoncxx::to_json(sort_doc.view()));
            }

            mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);
            if(querySpan) {
                querySpan->setSuccess(); // Query was submitted successfully
            }


            // 🎉 Span for processing all the results from the cursor
            auto processingSpan = creatures::observability->createChildOperationSpan("Database.processCursor", dbSpan);
            uint32_t documentsProcessed = 0;
            uint32_t documentsFailed = 0;

            // Go Mongo, go! 🎉
            for (auto &&doc: cursor) {
                documentsProcessed++;
                auto docSpan = creatures::observability->createChildOperationSpan("Database.processDocument", processingSpan);

                try {
                    // Convert BSON to JSON
                    auto conversionSpan = creatures::observability->createChildOperationSpan("BSON.toJson", docSpan);
                    std::string json_str = bsoncxx::to_json(doc);
                    if (conversionSpan) {
                        conversionSpan->setAttribute("bson.size_bytes", static_cast<int64_t>(doc.length()));
                        conversionSpan->setAttribute("json_string.size_bytes", static_cast<int64_t>(json_str.length()));
                        conversionSpan->setSuccess();
                    }

                    // Parse JSON string
                    auto parseSpan = creatures::observability->createChildOperationSpan("JSON.parse", docSpan);
                    nlohmann::json json_doc = nlohmann::json::parse(json_str);
                    if (parseSpan) {
                        parseSpan->setSuccess();
                    }

                    // Create AnimationMetadata from JSON
                    auto metadataSpan = creatures::observability->createChildOperationSpan("Animation.metadataFromJson", docSpan);
                    auto animationResult = animationMetadataFromJson(json_doc["metadata"]);

                    if (!animationResult.isSuccess()) {
                        documentsFailed++;
                        std::string errorMessage = fmt::format("Unable to parse JSON to AnimationMetadata: {}", animationResult.getError()->getMessage());
                        warn(errorMessage);

                        if(metadataSpan) metadataSpan->setError(errorMessage);
                        if(docSpan) docSpan->setError(errorMessage);

                        // Continue to the next document instead of failing the whole request
                        continue;
                    }

                    auto animationMetadata = animationResult.getValue().value();
                    animations.push_back(animationMetadata);

                    if(metadataSpan) {
                        metadataSpan->setAttribute("animation.title", animationMetadata.title);
                        metadataSpan->setSuccess();
                    }
                    if(docSpan) {
                        docSpan->setAttribute("animation.title", animationMetadata.title);
                        docSpan->setSuccess();
                    }
                } catch (const nlohmann::json::exception& e) {
                    documentsFailed++;
                    std::string errorMessage = fmt::format("JSON processing error for a document: {}", e.what());
                    warn(errorMessage);
                    if(docSpan) {
                        docSpan->recordException(e);
                        docSpan->setError(errorMessage);
                    }
                }
            } // End of cursor loop

            if(processingSpan) {
                processingSpan->setAttribute("documents.processed", static_cast<int64_t>(documentsProcessed));
                processingSpan->setAttribute("documents.failed", static_cast<int64_t>(documentsFailed));
                processingSpan->setSuccess();
            }
        }
        catch(const DataFormatException& e) {
            std::string errorMessage = fmt::format("Failed to get all animations: {}", e.what());
            warn(errorMessage);

            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                dbSpan->setError(errorMessage);
            }

            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InvalidData, errorMessage)};
        }
        catch(const mongocxx::exception &e) {
            std::string errorMessage = fmt::format("MongoDB Exception while loading animation: {}", e.what());
            critical(errorMessage);

            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                dbSpan->setError(errorMessage);
            }

            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
        }
        catch(const bsoncxx::exception &e) {
            std::string errorMessage = fmt::format("BSON error while attempting to load animations: {}", e.what());
            critical(errorMessage);

            if (dbSpan) {
                dbSpan->recordException(e);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
                dbSpan->setError(errorMessage);
            }

            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::InternalError, errorMessage)};
        }

        // Return a 404 if nothing as found
        if(animations.empty()) {
            std::string errorMessage = fmt::format("No animations found");
            warn(errorMessage);

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "NotFound");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            }

            return Result<std::vector<creatures::AnimationMetadata>>{ServerError(ServerError::NotFound, errorMessage)};
        }

        info("done loading the animation list");
        if (dbSpan) {
            dbSpan->setSuccess();
            dbSpan->setAttribute("animations.count", static_cast<int64_t>(animations.size()));
        }

        return Result<std::vector<creatures::AnimationMetadata>>{animations};
    }
}