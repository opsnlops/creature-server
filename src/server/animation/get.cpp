#include "server/config.h"

#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <mongocxx/client.hpp>

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

Result<json> Database::getAnimationJson(animationId_t animationId, std::shared_ptr<OperationSpan> parentSpan) {
    debug("attempting to get the JSON for an animation by ID: {}", animationId);

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimationJson", parentSpan);

    if (!parentSpan) {
        warn("no parent span provided for Database.getAnimationJson, creating a root span");
    }

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("animation.id", animationId);
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    if (animationId.empty()) {
        auto errorMessage = fmt::format("unable to get an animation because the id was empty");
        info(errorMessage);

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }

        return Result<json>{
            ServerError(ServerError::InvalidData, "unable to get an animation because the id was empty")};
    }

    try {
        // 🥕 Span for building the query filter
        auto filterSpan = creatures::observability->createChildOperationSpan("Database.buildFilter", dbSpan);
        if (filterSpan) {
            filterSpan->setAttribute("operation", "build_bson_filter");
            filterSpan->setAttribute("filter.field", "id");
            filterSpan->setAttribute("filter.value", animationId);
        }

        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << animationId;
        auto filter_doc = filter_builder.view();

        if (filterSpan) {
            filterSpan->setSuccess();
            filterSpan->setAttribute("filter.bson_size", static_cast<int64_t>(filter_doc.length()));
        }

        // 🐰 Span for getting the MongoDB collection
        auto collectionSpan = creatures::observability->createChildOperationSpan("Database.getCollection", dbSpan);
        if (collectionSpan) {
            collectionSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
            collectionSpan->setAttribute("operation", "get_collection");
        }

        auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto error = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("Database error while attempting to get an animation by ID: {}", error.getMessage());
            warn(errorMessage);

            if (collectionSpan) {
                collectionSpan->setError(errorMessage);
                collectionSpan->setAttribute("error.type", "DatabaseConnectionError");
            }

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "InvalidData");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }

            return Result<json>{error};
        }
        auto collection = collectionResult.getValue().value();

        if (collectionSpan) {
            collectionSpan->setSuccess();
        }

        // 🌟 The main MongoDB query span - this is where the magic (and time) happens!
        auto querySpan = creatures::observability->createChildOperationSpan("MongoDB.findOne", dbSpan);
        if (querySpan) {
            querySpan->setAttribute("db.system", "mongodb");
            querySpan->setAttribute("db.name", DB_NAME);
            querySpan->setAttribute("db.collection.name", ANIMATIONS_COLLECTION);
            querySpan->setAttribute("db.operation", "findOne");
            querySpan->setAttribute("db.query.filter", fmt::format("{{\"id\": \"{}\"}}", animationId));
            querySpan->setAttribute("db.query.type", "find_by_id");
        }

        // 🥕 The actual database query - this is the expensive part!
        auto maybe_result = collection.find_one(filter_doc);

        // 🐇 Span for processing the MongoDB result
        auto resultSpan = creatures::observability->createChildOperationSpan("Database.processResult", dbSpan);

        // Check if the document was found
        if (maybe_result) {
            if (querySpan) {
                querySpan->setSuccess();
                querySpan->setAttribute("db.result.found", true);
                querySpan->setAttribute("db.result.document_count", static_cast<int64_t>(1));
            }

            if (resultSpan) {
                resultSpan->setAttribute("operation", "bson_to_json_conversion");
                resultSpan->setAttribute("result.found", true);
            }

            // 🌟 BSON to JSON conversion span - this might be expensive too!
            auto conversionSpan = creatures::observability->createChildOperationSpan("BSON.toJson", dbSpan);
            if (conversionSpan) {
                conversionSpan->setAttribute("conversion.from", "bson");
                conversionSpan->setAttribute("conversion.to", "json");
            }

            // Convert BSON document to JSON using nlohmann::json
            bsoncxx::document::view view = maybe_result->view();
            auto bson_json_string = bsoncxx::to_json(view);

            if (conversionSpan) {
                conversionSpan->setAttribute("bson.size_bytes", static_cast<int64_t>(view.length()));
                conversionSpan->setAttribute("json_string.size_bytes", static_cast<int64_t>(bson_json_string.length()));
            }

            // 🥕 JSON parsing span - another potentially expensive operation
            auto parseSpan = creatures::observability->createChildOperationSpan("JSON.parse", dbSpan);
            if (parseSpan) {
                parseSpan->setAttribute("parser", "nlohmann_json");
                parseSpan->setAttribute("input.size_bytes", static_cast<int64_t>(bson_json_string.length()));
            }

            nlohmann::json json_result = nlohmann::json::parse(bson_json_string);

            if (parseSpan) {
                parseSpan->setSuccess();
                parseSpan->setAttribute("json.type", json_result.type_name());
                parseSpan->setAttribute("json.size", static_cast<int64_t>(json_result.size()));
                // Count nested objects/arrays for complexity
                if (json_result.contains("tracks") && json_result["tracks"].is_array()) {
                    parseSpan->setAttribute("animation.tracks_count",
                                            static_cast<int64_t>(json_result["tracks"].size()));
                }
                if (json_result.contains("metadata")) {
                    parseSpan->setAttribute("animation.has_metadata", true);
                }
            }

            if (conversionSpan) {
                conversionSpan->setSuccess();
                conversionSpan->setAttribute("json.parsed_size", static_cast<int64_t>(json_result.size()));
            }

            if (resultSpan) {
                resultSpan->setSuccess();
                resultSpan->setAttribute("json.final_size", static_cast<int64_t>(json_result.size()));
                resultSpan->setAttribute("conversion.successful", true);
            }

            if (dbSpan) {
                dbSpan->setSuccess();
                dbSpan->setAttribute("json_size", static_cast<int64_t>(json_result.size()));
                dbSpan->setAttribute("bson_size", static_cast<int64_t>(view.length()));
                dbSpan->setAttribute("json_string_size", static_cast<int64_t>(bson_json_string.length()));
                // Add some animation-specific metadata
                if (json_result.contains("metadata") && json_result["metadata"].contains("title")) {
                    dbSpan->setAttribute("animation.title", json_result["metadata"]["title"].get<std::string>());
                }
                if (json_result.contains("tracks") && json_result["tracks"].is_array()) {
                    dbSpan->setAttribute("animation.track_count", static_cast<int64_t>(json_result["tracks"].size()));
                }
            }

            return Result<json>{json_result};
        } else {
            // Document not found case
            std::string errorMessage = fmt::format("no animation id '{}' found", animationId);
            warn(errorMessage);

            if (querySpan) {
                querySpan->setSuccess(); // Query succeeded, just no results
                querySpan->setAttribute("db.result.found", false);
                querySpan->setAttribute("db.result.document_count", static_cast<int64_t>(0));
            }

            if (resultSpan) {
                resultSpan->setSuccess(); // Processing succeeded, just no document
                resultSpan->setAttribute("result.found", false);
                resultSpan->setAttribute("operation", "handle_not_found");
            }

            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "NotFound");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
                dbSpan->setAttribute("query.matched_documents", static_cast<int64_t>(0));
            }

            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }
    } catch (const mongocxx::exception &e) {
        std::string errorMessage =
            fmt::format("a MongoDB error happened while loading an animation by ID: {}", e.what());
        critical(errorMessage);

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "MongoDBException");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            dbSpan->setAttribute("error.mongodb_type", "mongocxx::exception");
            dbSpan->setAttribute("error.mongodb_message", e.what());
        }

        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage = fmt::format("JSON parsing error while loading an animation by ID: {}", e.what());
        critical(errorMessage);

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "JSONParsingException");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            dbSpan->setAttribute("error.json_type", "nlohmann::json::exception");
            dbSpan->setAttribute("error.json_message", e.what());
        }

        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("An unknown error happened while loading an animation by ID");
        critical(errorMessage);

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "UnknownException");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }

        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::Animation> Database::getAnimation(const animationId_t &animationId,
                                                    std::shared_ptr<OperationSpan> parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAnimation, creating a root span");
    }

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("animation.id", animationId);
        dbSpan->setAttribute("operation", "get_full_animation_object");
        dbSpan->setAttribute("database.system", "mongodb");
    }

    if (animationId.empty()) {
        std::string errorMessage = "unable to get an animation because the id was empty";
        warn(errorMessage);

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            dbSpan->setAttribute("validation.failed_on", "animation_id_empty");
        }

        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    // 🥕 Span for fetching raw JSON data
    auto fetchSpan = creatures::observability->createChildOperationSpan("Database.fetchAnimationJson", dbSpan);
    if (fetchSpan) {
        fetchSpan->setAttribute("operation", "fetch_raw_json");
        fetchSpan->setAttribute("animation.id", animationId);
        fetchSpan->setAttribute("target_function", "getAnimationJson");
    }

    // Go to the database and get the animation's raw JSON
    auto animationJson = getAnimationJson(animationId, dbSpan);

    if (!animationJson.isSuccess()) {
        auto error = animationJson.getError().value();
        std::string errorMessage =
            fmt::format("unable to get an animation by ID: {}", animationJson.getError()->getMessage());
        warn(errorMessage);

        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.source", "getAnimationJson");
            fetchSpan->setAttribute("error.type", "DatabaseFetchError");
        }

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            dbSpan->setAttribute("error.stage", "json_fetch");
        }

        return Result<creatures::Animation>{error};
    }

    // Successfully got JSON
    auto jsonData = animationJson.getValue().value();

    if (fetchSpan) {
        fetchSpan->setSuccess();
        fetchSpan->setAttribute("json.size", static_cast<int64_t>(jsonData.size()));
        fetchSpan->setAttribute("json.type", std::string(jsonData.type_name()));

        // Add some preliminary JSON analysis
        if (jsonData.contains("tracks") && jsonData["tracks"].is_array()) {
            fetchSpan->setAttribute("json.tracks_count", static_cast<int64_t>(jsonData["tracks"].size()));
        }
        if (jsonData.contains("metadata")) {
            fetchSpan->setAttribute("json.has_metadata", true);
            if (jsonData["metadata"].contains("number_of_frames")) {
                fetchSpan->setAttribute("json.metadata.frames",
                                        static_cast<int64_t>(jsonData["metadata"]["number_of_frames"]));
            }
        }
    }

    // 🌟 The expensive part - JSON to Animation object conversion!
    auto conversionSpan = creatures::observability->createChildOperationSpan("Animation.fromJson", dbSpan);
    if (conversionSpan) {
        conversionSpan->setAttribute("operation", "json_to_animation_conversion");
        conversionSpan->setAttribute("converter_function", "animationFromJson");
        conversionSpan->setAttribute("input.json_size", static_cast<int64_t>(jsonData.size()));
        conversionSpan->setAttribute("input.json_type", std::string(jsonData.type_name()));

        // Pre-conversion analysis for debugging
        if (jsonData.contains("tracks") && jsonData["tracks"].is_array()) {
            conversionSpan->setAttribute("conversion.tracks_to_process",
                                         static_cast<int64_t>(jsonData["tracks"].size()));

            // Calculate total frame data to process
            int64_t totalFrames = 0;
            for (const auto &track : jsonData["tracks"]) {
                if (track.contains("frames") && track["frames"].is_array()) {
                    totalFrames += track["frames"].size();
                }
            }
            conversionSpan->setAttribute("conversion.total_frames_to_process", totalFrames);
        }

        if (jsonData.contains("metadata")) {
            conversionSpan->setAttribute("conversion.has_metadata", true);
            if (jsonData["metadata"].contains("title")) {
                conversionSpan->setAttribute("conversion.animation_title",
                                             jsonData["metadata"]["title"].get<std::string>());
            }
        }
    }

    // Convert it to an Animation object (if possible) - THIS is where the 5.57ms is spent!
    auto result = animationFromJson(jsonData);

    if (!result.isSuccess()) {
        auto error = result.getError().value();
        std::string errorMessage = fmt::format("unable to get an animation by ID: {}", result.getError()->getMessage());
        warn(errorMessage);

        if (conversionSpan) {
            conversionSpan->setError(errorMessage);
            conversionSpan->setAttribute("error.source", "animationFromJson");
            conversionSpan->setAttribute("error.type", "AnimationConversionError");
            conversionSpan->setAttribute("conversion.failed", true);
        }

        if (dbSpan) {
            dbSpan->setError(errorMessage);
            dbSpan->setAttribute("error.type", "InvalidData");
            dbSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            dbSpan->setAttribute("error.stage", "animation_conversion");
        }

        return Result<creatures::Animation>{error};
    }

    // 🐇 Successful conversion - let's analyze what we created!
    auto animation = result.getValue().value();

    if (conversionSpan) {
        conversionSpan->setSuccess();
        conversionSpan->setAttribute("conversion.successful", true);

        // Post-conversion analysis
        conversionSpan->setAttribute("output.animation_id", animation.id);
        conversionSpan->setAttribute("output.animation_title", animation.metadata.title);
        conversionSpan->setAttribute("output.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        conversionSpan->setAttribute("output.milliseconds_per_frame",
                                     static_cast<int64_t>(animation.metadata.milliseconds_per_frame));
        conversionSpan->setAttribute("output.number_of_frames",
                                     static_cast<int64_t>(animation.metadata.number_of_frames));
        conversionSpan->setAttribute("output.has_sound_file", !animation.metadata.sound_file.empty());
        conversionSpan->setAttribute("output.multitrack_audio", animation.metadata.multitrack_audio);

        // Calculate total frame data converted
        int64_t totalFramesConverted = 0;
        for (const auto &track : animation.tracks) {
            totalFramesConverted += track.frames.size();
        }
        conversionSpan->setAttribute("output.total_frames_converted", totalFramesConverted);

        // Estimate memory usage (rough approximation)
        int64_t estimatedMemoryBytes = 0;
        for (const auto &track : animation.tracks) {
            for (const auto &frame : track.frames) {
                estimatedMemoryBytes += frame.length(); // Base64 string length
            }
        }
        conversionSpan->setAttribute("output.estimated_frame_data_bytes", estimatedMemoryBytes);
    }

    // 🥕 Final validation and result preparation span
    auto finalizationSpan = creatures::observability->createChildOperationSpan("Animation.finalize", dbSpan);
    if (finalizationSpan) {
        finalizationSpan->setAttribute("operation", "result_finalization");
        finalizationSpan->setAttribute("animation.id", animation.id);
        finalizationSpan->setAttribute("animation.title", animation.metadata.title);

        // Final validation checks
        bool validAnimation = !animation.id.empty() && !animation.metadata.title.empty();
        finalizationSpan->setAttribute("validation.animation_valid", validAnimation);
        finalizationSpan->setAttribute("validation.has_tracks", !animation.tracks.empty());

        if (!animation.tracks.empty()) {
            // Check if all tracks have the same creature references
            std::unordered_set<std::string> uniqueCreatureIds;
            for (const auto &track : animation.tracks) {
                uniqueCreatureIds.insert(track.creature_id);
            }
            finalizationSpan->setAttribute("validation.unique_creatures_count",
                                           static_cast<int64_t>(uniqueCreatureIds.size()));
        }

        finalizationSpan->setSuccess();
    }

    if (dbSpan) {
        dbSpan->setSuccess();
        dbSpan->setAttribute("animation.title", animation.metadata.title);
        dbSpan->setAttribute("animation.id", animation.id);
        dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        dbSpan->setAttribute("animation.total_frames", static_cast<int64_t>(animation.metadata.number_of_frames));
        dbSpan->setAttribute("animation.duration_ms", static_cast<int64_t>(animation.metadata.number_of_frames *
                                                                           animation.metadata.milliseconds_per_frame));
        dbSpan->setAttribute("animation.has_sound", !animation.metadata.sound_file.empty());
        dbSpan->setAttribute("processing.stages_completed", static_cast<int64_t>(3)); // fetch, convert, finalize

        // Performance insights
        if (animation.tracks.size() > 10) {
            dbSpan->setAttribute("performance.large_track_count", true);
        }
        if (animation.metadata.number_of_frames > 1000) {
            dbSpan->setAttribute("performance.large_frame_count", true);
        }
    }

    return Result<creatures::Animation>{animation};
}
} // namespace creatures
