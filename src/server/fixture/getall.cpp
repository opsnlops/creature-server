
#include "server/config.h"

#include <string>
#include <vector>

#include "exception/exception.h"
#include "model/DmxFixture.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "spdlog/spdlog.h"
#include <fmt/format.h>

#include <mongocxx/client.hpp>
#include <mongocxx/cursor.hpp>
#include <mongocxx/options/find.hpp>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

#include "server/namespace-stuffs.h"

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;
using bsoncxx::builder::stream::document;

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
extern std::shared_ptr<ObservabilityManager> observability;

Result<std::vector<creatures::DmxFixture>> Database::getAllFixtures(const std::shared_ptr<OperationSpan> &parentSpan) {

    auto dbSpan = creatures::observability->createChildOperationSpan("Database.getAllFixtures", parentSpan);

    if (dbSpan) {
        dbSpan->setAttribute("database.collection", FIXTURES_COLLECTION);
        dbSpan->setAttribute("database.operation", "find");
        dbSpan->setAttribute("database.system", "mongodb");
        dbSpan->setAttribute("database.name", DB_NAME);
    }

    info("attempting to get all of the DmxFixtures");

    auto fixtureList = std::vector<DmxFixture>{};

    try {
        document query_doc{};
        document sort_doc{};
        sort_doc << "name" << 1;

        auto collectionResult = getCollection(FIXTURES_COLLECTION);
        if (!collectionResult.isSuccess()) {
            auto err = collectionResult.getError().value();
            std::string errorMessage = fmt::format("unable to get the fixture collection: {}", err.getMessage());
            critical(errorMessage);
            if (dbSpan) {
                dbSpan->setError(errorMessage);
                dbSpan->setAttribute("error.type", "DatabaseError");
                dbSpan->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
            }
            return Result<std::vector<DmxFixture>>{err};
        }
        auto collection = collectionResult.getValue().value();

        auto mongoSpan = creatures::observability->createChildOperationSpan("getAllFixtures::mongo-query", dbSpan);
        mongocxx::options::find opts;
        opts.sort(sort_doc.view());
        mongocxx::cursor cursor = collection.find(query_doc.view(), opts);

        for (auto doc : cursor) {
            auto fixtureSpan =
                creatures::observability->createChildOperationSpan("getAllFixtures::create-fixture", mongoSpan);

            auto jsonResult = JsonParser::bsonToJson(doc, "fixture document", fixtureSpan);
            if (!jsonResult.isSuccess()) {
                continue;
            }
            json j = jsonResult.getValue().value();

            auto result = fixtureFromJson(j, fixtureSpan);
            if (!result.isSuccess()) {
                auto err = result.getError().value();
                std::string errorMessage =
                    fmt::format("Data format error while trying to get all of the fixtures: {}", err.getMessage());
                critical(errorMessage);
                if (fixtureSpan) {
                    fixtureSpan->setError(errorMessage);
                    fixtureSpan->setAttribute("error.type", "DataFormatException");
                    fixtureSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
                }
                return Result<std::vector<DmxFixture>>{err};
            }
            fixtureList.push_back(result.getValue().value());
            fixtureCache->put(result.getValue().value().id, result.getValue().value());

            fixtureSpan->setAttribute("fixture.id", result.getValue().value().id);
            fixtureSpan->setAttribute("fixture.name", result.getValue().value().name);
            fixtureSpan->setSuccess();
        }
        mongoSpan->setAttribute("fixtures.count", static_cast<int64_t>(fixtureList.size()));
        mongoSpan->setSuccess();

        debug("found {} fixtures", fixtureList.size());
        if (dbSpan) {
            dbSpan->setAttribute("fixtures.count", static_cast<int64_t>(fixtureList.size()));
            dbSpan->setSuccess();
        }
        return Result<std::vector<DmxFixture>>{fixtureList};

    } catch (const DataFormatException &e) {
        std::string errorMessage =
            fmt::format("Data format error while trying to get all of the fixtures: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setAttribute("error.type", "DataFormatException");
            dbSpan->setError(errorMessage);
        }
        return Result<std::vector<DmxFixture>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const DatabaseError &e) {
        std::string errorMessage =
            fmt::format("A database error happened while getting all of the fixtures: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setAttribute("error.type", "DatabaseError");
            dbSpan->setError(errorMessage);
        }
        return Result<std::vector<DmxFixture>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Failed to get all fixtures: {}", e.what());
        error(errorMessage);
        if (dbSpan) {
            dbSpan->recordException(e);
            dbSpan->setAttribute("error.type", "std::exception");
            dbSpan->setError(errorMessage);
        }
        return Result<std::vector<DmxFixture>>{ServerError(ServerError::InternalError, errorMessage)};
    } catch (...) {
        std::string errorMessage = "Failed to get all fixtures: unknown error";
        error(errorMessage);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
        }
        return Result<std::vector<DmxFixture>>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

} // namespace creatures
