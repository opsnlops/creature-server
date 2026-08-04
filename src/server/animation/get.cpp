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

    debug("attempting to get the JSON for an animation by ID: {}", animationId);

    if (animationId.empty()) {
        std::string errorMessage = "unable to get an animation because the id was empty";
        info(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<json>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        std::string errorMessage =
            fmt::format("Database error while attempting to get an animation by ID: {}", err.getMessage());
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "DatabaseError", err.getCode());
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
            recordSpanError(dbSpan, errorMessage, "NotFound", ServerError::NotFound);
            return Result<json>{ServerError(ServerError::NotFound, errorMessage)};
        }

        auto convertSpan = creatures::observability->createChildOperationSpan("getAnimationJson.bson-to-json", dbSpan);
        auto jsonResult =
            JsonParser::bsonToJson(maybe_result->view(), fmt::format("animation {}", animationId), convertSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            warn("Failed to convert BSON to JSON for animation {}: {}", animationId, err.getMessage());
            recordSpanError(dbSpan, err.getMessage(), "JsonParsingException", err.getCode());
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
        recordSpanError(dbSpan, errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<json>{ServerError(ServerError::DatabaseError, errorMessage)};
    } catch (const nlohmann::json::exception &e) {
        std::string errorMessage =
            fmt::format("JSON parsing error while loading animation {}: {}", animationId, e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        recordSpanError(dbSpan, errorMessage, "JsonParsingException", ServerError::InternalError);
        return Result<json>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = fmt::format("Unknown error while loading animation {}", animationId);
        critical(errorMessage);
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
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

    if (animationId.empty()) {
        std::string errorMessage = "unable to get an animation because the id was empty";
        warn(errorMessage);
        recordSpanError(dbSpan, errorMessage, "InvalidData", ServerError::InvalidData);
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
        recordSpanError(dbSpan, errorMessage, etype, err.getCode());
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
        recordSpanError(dbSpan, errorMessage, "InvalidData", err.getCode());
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
Database::findAnimationIdBySourceScriptId(const std::string &scriptId, const std::string &stageId,
                                          const std::shared_ptr<OperationSpan> &parentSpan) {
    auto dbSpan =
        creatures::observability->createChildOperationSpan("Database.findAnimationIdBySourceScriptId", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find_one");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("script.id", scriptId);
        dbSpan->setAttribute("stage.id", stageId);
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
        //
        // The key is (script, stage), not script alone (#119): rendering the
        // same scene for the travel stage must produce a SECOND animation
        // rather than clobbering the mainstage one, so both renditions stay
        // live. An empty stageId matches documents with no stage — which is
        // every animation rendered before stages existed, so the old
        // no-stage behavior is preserved exactly.
        auto filterBuilder = bsoncxx::builder::stream::document{};
        filterBuilder << "metadata.source_script_id" << scriptId;
        if (stageId.empty()) {
            filterBuilder << "metadata.source_stage_id" << bsoncxx::builder::stream::open_document << "$in"
                          << bsoncxx::builder::stream::open_array << bsoncxx::types::b_null{} << ""
                          << bsoncxx::builder::stream::close_array << bsoncxx::builder::stream::close_document;
        } else {
            filterBuilder << "metadata.source_stage_id" << stageId;
        }
        auto filter = filterBuilder << bsoncxx::builder::stream::finalize;
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

Result<std::vector<Database::StageAnimationRef>>
Database::listAnimationsBySourceStageId(const std::string &stageId, int64_t stageUpdatedAt,
                                        const std::shared_ptr<OperationSpan> &parentSpan) {
    auto dbSpan =
        creatures::observability->createChildOperationSpan("Database.listAnimationsBySourceStageId", parentSpan);
    if (dbSpan) {
        dbSpan->setAttribute("database.collection", ANIMATIONS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
        dbSpan->setAttribute("stage.id", stageId);
    }

    std::vector<StageAnimationRef> refs;
    if (stageId.empty()) {
        if (dbSpan)
            dbSpan->setSuccess();
        return Result<std::vector<StageAnimationRef>>{refs};
    }

    auto collectionResult = getCollection(ANIMATIONS_COLLECTION);
    if (!collectionResult.isSuccess()) {
        auto err = collectionResult.getError().value();
        recordSpanError(dbSpan, err.getMessage(), "DatabaseError", err.getCode());
        return Result<std::vector<StageAnimationRef>>{err};
    }
    auto collection = collectionResult.getValue().value();

    try {
        // Project only the provenance we need. Animation documents carry every
        // base64 frame for every track, so pulling whole documents here would
        // move megabytes to answer a question about a handful of scalars.
        auto filter = bsoncxx::builder::stream::document{} << "metadata.source_stage_id" << stageId
                                                           << bsoncxx::builder::stream::finalize;
        auto projection = bsoncxx::builder::stream::document{} << "id" << 1 << "metadata.title" << 1
                                                               << "metadata.source_script_id" << 1
                                                               << "metadata.source_stage_updated_at" << 1 << "_id" << 0
                                                               << bsoncxx::builder::stream::finalize;
        mongocxx::options::find opts;
        opts.projection(projection.view());

        auto cursor = collection.find(filter.view(), opts);
        for (auto doc : cursor) {
            StageAnimationRef ref;
            auto idElement = doc["id"];
            if (!idElement || idElement.type() != bsoncxx::type::k_utf8) {
                warn("listAnimationsBySourceStageId: matching doc has no string 'id' field — ignoring");
                continue;
            }
            ref.animation_id = std::string(idElement.get_string().value);

            if (auto metadata = doc["metadata"]; metadata && metadata.type() == bsoncxx::type::k_document) {
                auto metadataView = metadata.get_document().view();
                if (auto title = metadataView["title"]; title && title.type() == bsoncxx::type::k_utf8) {
                    ref.title = std::string(title.get_string().value);
                }
                if (auto script = metadataView["source_script_id"]; script && script.type() == bsoncxx::type::k_utf8) {
                    ref.source_script_id = std::string(script.get_string().value);
                }
                if (auto stamp = metadataView["source_stage_updated_at"]; stamp) {
                    if (stamp.type() == bsoncxx::type::k_int64) {
                        ref.source_stage_updated_at = stamp.get_int64().value;
                    } else if (stamp.type() == bsoncxx::type::k_int32) {
                        ref.source_stage_updated_at = stamp.get_int32().value;
                    }
                }
            }

            // Rendered against an older version of this stage than the one
            // currently stored, so its head aiming no longer matches reality.
            ref.stale = ref.source_stage_updated_at < stageUpdatedAt;
            refs.push_back(std::move(ref));
        }

        // Most out-of-date first: that's the order you want to act on.
        std::sort(refs.begin(), refs.end(), [](const StageAnimationRef &a, const StageAnimationRef &b) {
            return a.source_stage_updated_at < b.source_stage_updated_at;
        });

        if (dbSpan) {
            dbSpan->setAttribute("animations.count", static_cast<int64_t>(refs.size()));
            dbSpan->setAttribute("animations.stale_count",
                                 static_cast<int64_t>(std::count_if(
                                     refs.begin(), refs.end(), [](const StageAnimationRef &r) { return r.stale; })));
            dbSpan->setSuccess();
        }
        return Result<std::vector<StageAnimationRef>>{refs};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to list animations for stage {}: {}", stageId, e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
        }
        recordSpanError(dbSpan, errorMessage, "std::exception", ServerError::InternalError);
        return Result<std::vector<StageAnimationRef>>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
