//
// universe-memory.cpp - Remembering which universe each creature plays on
//
// Playback targets a universe, and that assignment used to live only in an
// in-memory, add-only map filled by POST /api/v1/creature/register — which
// controllers call at their own startup. A server restart forgot everything,
// so after a deploy a dialog whose participant had not re-registered was
// refused outright. That is wrong twice over: a creature's audio is a channel
// in the 17-channel room mix and belongs in the room whether or not its body
// is powered on, and a creature taken down for maintenance is still assigned
// to the universe it was last playing on (issue #231).
//

#include "server/config.h"

#include <string>
#include <unordered_map>

#include "server/creature-server.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

using bsoncxx::builder::stream::close_document;
using bsoncxx::builder::stream::document;
using bsoncxx::builder::stream::open_document;

Result<void> Database::setCreatureLastKnownUniverse(const creatureId_t &creatureId, universe_t universe,
                                                    const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.setCreatureLastKnownUniverse, creating a root span");
    }
    auto span = creatures::observability->createChildOperationSpan("Database.setCreatureLastKnownUniverse", parentSpan);
    if (span) {
        span->setAttribute("database.collection", CREATURES_COLLECTION);
        span->setAttribute("database.operation", "update_one");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
        span->setAttribute("creature.id", creatureId);
        span->setAttribute("creature.universe", static_cast<int64_t>(universe));
    }

    if (creatureId.empty()) {
        const std::string errorMessage = "setCreatureLastKnownUniverse called with empty creatureId";
        warn(errorMessage);
        recordSpanError(span, errorMessage, "InvalidData", ServerError::InvalidData);
        return Result<void>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    try {
        auto collectionResult = getCollection(CREATURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return Result<void>{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        document filter_builder;
        filter_builder << "id" << creatureId;

        document update_builder;
        update_builder << "$set" << open_document << CREATURE_LAST_KNOWN_UNIVERSE_FIELD
                       << static_cast<int64_t>(universe) << close_document;

        const auto result = collection.update_one(filter_builder.view(), update_builder.view());
        if (!result) {
            const std::string errorMessage =
                fmt::format("No acknowledgement storing the last known universe for creature {}", creatureId);
            warn(errorMessage);
            recordSpanError(span, errorMessage, "DatabaseError", ServerError::InternalError);
            return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
        }

        // A creature that isn't in the database yet is not an error here: the
        // registration path upserts the creature itself, and this is only the
        // remembered runtime state alongside it.
        if (span) {
            span->setAttribute("database.matched_count", static_cast<int64_t>(result->matched_count()));
            span->setSuccess();
        }
        debug("remembered creature {} on universe {}", creatureId, universe);
        return Result<void>{};

    } catch (const std::exception &e) {
        const auto errorMessage =
            fmt::format("Failed to store the last known universe for creature {}: {}", creatureId, e.what());
        warn(errorMessage);
        recordSpanError(span, errorMessage, "DatabaseError", ServerError::InternalError);
        return Result<void>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<std::unordered_map<creatureId_t, universe_t>>
Database::getCreatureLastKnownUniverses(const std::shared_ptr<OperationSpan> &parentSpan) {
    using ResultType = Result<std::unordered_map<creatureId_t, universe_t>>;

    auto span =
        creatures::observability->createChildOperationSpan("Database.getCreatureLastKnownUniverses", parentSpan);
    if (span) {
        span->setAttribute("database.collection", CREATURES_COLLECTION);
        span->setAttribute("database.operation", "find");
        span->setAttribute("database.system", "mongodb");
        span->setAttribute("database.name", DB_NAME);
    }

    std::unordered_map<creatureId_t, universe_t> remembered;

    try {
        auto collectionResult = getCollection(CREATURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            recordSpanError(span, err.getMessage(), "DatabaseError", err.getCode());
            return ResultType{err};
        }
        auto collectionLease = collectionResult.getValue().value();
        auto &collection = collectionLease->collection();

        // Only creatures that have ever been seen on a universe.
        document query_doc;
        query_doc << CREATURE_LAST_KNOWN_UNIVERSE_FIELD << open_document << "$exists" << true << close_document;

        auto cursor = collection.find(query_doc.view());
        for (auto &&doc : cursor) {
            const auto idElement = doc["id"];
            const auto universeElement = doc[CREATURE_LAST_KNOWN_UNIVERSE_FIELD];
            if (!idElement || idElement.type() != bsoncxx::type::k_string) {
                continue;
            }
            if (!universeElement) {
                continue;
            }

            // Mongo may hand back either width depending on how it was written.
            std::int64_t universeValue = 0;
            if (universeElement.type() == bsoncxx::type::k_int64) {
                universeValue = universeElement.get_int64().value;
            } else if (universeElement.type() == bsoncxx::type::k_int32) {
                universeValue = universeElement.get_int32().value;
            } else {
                continue;
            }
            if (universeValue <= 0) {
                continue;
            }

            remembered.emplace(std::string(idElement.get_string().value), static_cast<universe_t>(universeValue));
        }

        if (span) {
            span->setAttribute("creatures.remembered", static_cast<int64_t>(remembered.size()));
            span->setSuccess();
        }
        return ResultType{std::move(remembered)};

    } catch (const std::exception &e) {
        const auto errorMessage = fmt::format("Failed to read remembered creature universes: {}", e.what());
        warn(errorMessage);
        recordSpanError(span, errorMessage, "DatabaseError", ServerError::InternalError);
        return ResultType{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
