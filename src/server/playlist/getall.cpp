
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

#include "server/namespace-stuffs.h"

using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;

// Conforms to docs/database-observability.md (issue #17).

Result<std::vector<creatures::Playlist>> Database::getAllPlaylists(const std::shared_ptr<OperationSpan> &parentSpan) {
    if (!parentSpan) {
        warn("no parent span provided for Database.getAllPlaylists, creating a root span");
    }
    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAllPlaylists", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", PLAYLISTS_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    auto setSpanError = [&](const std::string &msg, const std::string &type, ServerError::Code code) {
        if (dbSpan) {
            dbSpan->setError(msg);
            dbSpan->setAttribute("error.type", type);
            dbSpan->setAttribute("error.code", static_cast<int64_t>(code));
        }
    };

    debug("attempting to get all of the playlists");

    std::vector<creatures::Playlist> playlists;

    try {
        auto collectionResult = getCollection(PLAYLISTS_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage =
                fmt::format("database error while getting all of the playlists: {}", err.getMessage());
            warn(errorMessage);
            setSpanError(errorMessage, "DatabaseError", err.getCode());
            return Result<std::vector<creatures::Playlist>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("getAllPlaylists.mongoQuery", dbSpan);
        document query_doc{};
        document projection_doc{};
        document sort_doc{};
        sort_doc << "name" << 1;

        mongocxx::options::find findOptions{};
        findOptions.projection(projection_doc.view());
        findOptions.sort(sort_doc.view());

        mongocxx::cursor cursor = collection.find(query_doc.view(), findOptions);

        for (auto &&doc : cursor) {
            auto playlistSpan =
                creatures::observability->createChildOperationSpan("getAllPlaylists.create-playlist", mongoSpan);

            auto jsonResult = JsonParser::bsonToJson(doc, "playlist document", playlistSpan);
            if (!jsonResult.isSuccess()) {
                auto err = jsonResult.getError().value();
                if (playlistSpan) {
                    playlistSpan->setError(err.getMessage());
                    playlistSpan->setAttribute("error.type", "JsonParsingException");
                    playlistSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                continue;
            }
            nlohmann::json json_doc = jsonResult.getValue().value();

            auto playlistResult = playlistFromJson(json_doc, playlistSpan);
            if (!playlistResult.isSuccess()) {
                auto err = playlistResult.getError().value();
                std::string errorMessage = fmt::format("Unable to parse playlist JSON: {}", err.getMessage());
                warn(errorMessage);
                if (playlistSpan) {
                    playlistSpan->setError(errorMessage);
                    playlistSpan->setAttribute("error.type", "DataFormatException");
                    playlistSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
                }
                setSpanError(errorMessage, "DataFormatException", err.getCode());
                return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InvalidData, errorMessage)};
            }

            auto playlist = playlistResult.getValue().value();
            playlists.push_back(playlist);
            if (playlistSpan) {
                playlistSpan->setAttribute("playlist.id", playlist.id);
                playlistSpan->setAttribute("playlist.name", playlist.name);
                playlistSpan->setSuccess();
            }
        }
        if (mongoSpan) {
            mongoSpan->setAttribute("playlists.count", static_cast<int64_t>(playlists.size()));
            mongoSpan->setSuccess();
        }
    } catch (const DataFormatException &e) {
        std::string errorMessage = fmt::format("Failed to get all playlists: {}", e.what());
        warn(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "DataFormatException", ServerError::InvalidData);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InvalidData, errorMessage)};
    } catch (const mongocxx::exception &e) {
        std::string errorMessage = fmt::format("MongoDB Exception while loading the playlists: {}", e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "MongoDBException", ServerError::DatabaseError);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const bsoncxx::exception &e) {
        std::string errorMessage = fmt::format("BSON error while loading the playlists: {}", e.what());
        critical(errorMessage);
        if (dbSpan)
            dbSpan->recordException(e);
        setSpanError(errorMessage, "JsonParsingException", ServerError::InternalError);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    if (playlists.empty()) {
        std::string errorMessage = "No playlists found";
        warn(errorMessage);
        setSpanError(errorMessage, "NotFound", ServerError::NotFound);
        return Result<std::vector<creatures::Playlist>>{ServerError(ServerError::NotFound, errorMessage)};
    }

    if (dbSpan) {
        dbSpan->setAttribute("playlists.count", static_cast<int64_t>(playlists.size()));
        dbSpan->setSuccess();
    }
    info("done loading {} playlists", playlists.size());
    return Result<std::vector<creatures::Playlist>>{playlists};
}

} // namespace creatures
