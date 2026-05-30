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
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17). Was previously
// over-instrumented with sub-spans like `Database.buildFilter`, `MongoDB.findOne`,
// `BSON.toJson`, `JSON.parse`, etc. — those weren't useful in Honeycomb and
// just added noise. Standard names + the one mongoQuery wrapper give us
// everything we need to slice latency.

Result<json> Database::getAnimationJson(const animationId_t &animationId,
                                        const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAnimationJson, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimationJson", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.id", animationId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    debug("attempting to get the JSON for an animation by ID: {}", animationId);

    if (animationId.empty()) {
        std::string errorMessage = "unable to get an animation because the id was empty";
        info(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage =
            fmt::format("Database error while attempting to get an animation by ID: {}", err.getMessage());
        warn(errorMessage);
        setSpanError(errorMessage, "DatabaseError", err.getCode());
        return Result<json>{err};
    }
    auto collection = collectionResult.getValue().value();

    std::shared_ptr<OperationSpan> mongoSpan;
    try {
        mongoSpan = creatures::observability->createChildOperationSpan("getAnimationJson.mongoQuery", dbSpan);

        bsoncxx::builder::stream::document filter_builder;
        filter_builder << "id" << animationId;
        auto maybe_result = collection.find_one(filter_builder.view());
        if (mongoSpan)
            mongoSpan->setSuccess();

        if (!maybe_result) {
            std::string errorMessage = fmt::format("no animation id '{}' found", animationId);
            warn(errorMessage);
            setSpanError(errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getAnimationJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("animation {}", animationId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for animation {}: {}", animationId, err.getMessage());
            setSpanError(err.getMessage(), "JsonParsingException", err.getCode());
            return jsonResult;
        }
        nlohmann::json j = jsonResult.getValue().value();

        if (dbSpan) {
            dbSpan->setAttribute("db.response_size_bytes", static_cast<int64_t>(j.dump().length()));
            // Useful filterable attributes — what would an oncall query on?
            if (j.contains("metadata") && j["metadata"].contains("title") && j["metadata"]["title"].is_string()) {
                dbSpan->setAttribute("animation.title", j["metadata"]["title"].get<std::string>());
            }
            if (j.contains("tracks") && j["tracks"].is_array()) {
                dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(j["tracks"].size()));
            }
            dbSpan->setSuccess();
        }
        return Result<json>{j};

    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("MongoDB error while loading animation {}: {}", animationId, e.what());
        critical(errorMessage);
        if (mongoSpan) {
            mongoSpan->recordException(e);
            mongoSpan->setError(errorMessage);
        }
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("JSON parsing error while loading animation {}: {}", animationId, e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "JsonParsingException", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while loading animation {}", animationId);
        critical(errorMessage);
        setSpanError(errorMessage, "std::exception", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<creatures::Animation> Database::getAnimation(const animationId_t &animationId,
                                                    const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAnimation, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAnimation", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("animation.id", animationId);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    if (animationId.empty()) {
        std::string errorMessage = "unable to get an animation because the id was empty";
        warn(errorMessage);
        setSpanError(errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<creatures::Animation>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto jsonSpan = creatures::observability->createChildOperationSpan("getAnimation.getAnimationJson", dbSpan);
    auto animationJson = getAnimationJson(animationId, jsonSpan);
    if (!animationJson.isSuccess()) {
        auto err = animationJson.getError().value();
        std::string errorMessage = fmt::format("unable to get an animation by ID: {}", err.getMessage());
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
        setSpanError(errorMessage, etype, err.getCode());
        return Result<creatures::Animation>{err};
    }
    if (jsonSpan)
        jsonSpan->setSuccess();

    auto fetchSpan = creatures::observability->createChildOperationSpan("getAnimation.animationFromJson", dbSpan);
    auto result = animationFromJson(animationJson.getValue().value());
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        std::string errorMessage = fmt::format("unable to get an animation by ID: {}", err.getMessage());
        warn(errorMessage);
        if (fetchSpan) {
            fetchSpan->setError(errorMessage);
            fetchSpan->setAttribute("error.type", "InvalidData");
            fetchSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        setSpanError(errorMessage, "InvalidData", err.getCode());
        return Result<creatures::Animation>{err};
    }
    if (fetchSpan)
        fetchSpan->setSuccess();

    auto animation = result.getValue().value();
    if (dbSpan) {
        dbSpan->setAttribute("animation.title", animation.metadata.title);
        dbSpan->setAttribute("animation.tracks_count", static_cast<int64_t>(animation.tracks.size()));
        dbSpan->setAttribute("animation.number_of_frames", static_cast<int64_t>(animation.metadata.number_of_frames));
        dbSpan->setAttribute("animation.duration_ms", static_cast<int64_t>(animation.metadata.number_of_frames *
                                                                           animation.metadata.milliseconds_per_frame));
        dbSpan->setAttribute("animation.has_sound", !animation.metadata.sound_file.empty());
        dbSpan->setSuccess();
    }
    return Result<creatures::Animation>{animation};
}

Result<std::optional<animationId_t>>
Database::findAnimationIdBySourceScriptId(const std::string &scriptId,
                                          const std::shared_ptr<OperationSpan> &parentSpan) {
    auto dbSpan =
        creatures::observability->createChildOperationSpan("Database.findAnimationIdBySourceScriptId", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("script.id", scriptId);
    }

    if (scriptId.empty()) {
        if (dbSpan)
            dbSpan->setSuccess();
        return Result<std::optional<animationId_t>>{std::optional<animationId_t>{}};
    }

    auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        if (dbSpan)
            dbSpan->setError(err.getMessage());
        return Result<std::optional<animationId_t>>{err};
    }
    auto collection = collectionResult.getValue().value();

    try {
        // Project just `id` — we don't need the (potentially large) tracks blob
        // for a dedupe check.
        auto filter = bsoncxx::builder::stream::document{} << "metadata.source_script_id" << scriptId
                                                           << bsoncxx::builder::stream::finalize;
        auto projection = bsoncxx::builder::stream::document{} << "id" << 1 << "_id" << 0
                                                               << bsoncxx::builder::stream::finalize;
        mongocxx::options::find opts;
        opts.projection(projection.view());

        auto maybe = collection.find_one(filter.view(), opts);
        if (!maybe) {
            if (dbSpan) {
                dbSpan->setAttribute("dedupe.found", false);
                dbSpan->setSuccess();
            }
            return Result<std::optional<animationId_t>>{std::optional<animationId_t>{}};
        }

        auto view = maybe->view();
        auto idElem = view["id"];
        if (!idElem || idElem.type() != bsoncxx::type::k_utf8) {
            warn("findAnimationIdBySourceScriptId: matching doc has no string 'id' field — ignoring");
            if (dbSpan)
                dbSpan->setSuccess();
            return Result<std::optional<animationId_t>>{std::optional<animationId_t>{}};
        }
        std::string foundId{idElem.get_string().value.data(), idElem.get_string().value.size()};
        if (dbSpan) {
            dbSpan->setAttribute("dedupe.found", true);
            dbSpan->setAttribute("animation.id", foundId);
            dbSpan->setSuccess();
        }
        return Result<std::optional<animationId_t>>{std::optional<animationId_t>{std::move(foundId)}};

    } catch (const mongocxx::exception &e) {
        std::string msg =
            fmt::format("MongoDB error finding animation by source_script_id '{}': {}", scriptId, e.what());
        error(msg);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(msg);
        }
        return Result<std::optional<animationId_t>>{ServerError(ServerError::DatabaseError, msg)};
    } catch (const std::exception &e) {
        std::string msg = fmt::format("Error finding animation by source_script_id '{}': {}", scriptId, e.what());
        error(msg);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setError(msg);
        }
        return Result<std::optional<animationId_t>>{ServerError(ServerError::InternalError, msg)};
    }
}

} // namespace creatures
