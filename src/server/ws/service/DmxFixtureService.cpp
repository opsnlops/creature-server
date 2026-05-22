
#include <oatpp/core/Types.hpp>

#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "exception/exception.h"
#include "model/DmxFixture.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/fixture/FixtureBindingDispatcher.h"
#include "server/fixture/FixturePatternRunner.h"
#include "server/fixture/FixturePatternTickEvent.h"
#include "server/ws/dto/FixtureConfigValidationDto.h"
#include "server/ws/dto/ListDto.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "DmxFixtureService.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
extern std::shared_ptr<ObjectCache<fixtureId_t, universe_t>> fixtureUniverseMap;
extern std::shared_ptr<FixturePatternRunner> fixturePatternRunner;
extern std::shared_ptr<EventLoop> eventLoop;
} // namespace creatures

namespace {

// Per-fixture mutex map for serializing universe assignment ops. Two concurrent
// PUT /universe calls for the same fixture could land DB writes in order A→B
// and cache writes in order B→A, leaving the DB and the runtime map disagreeing
// on which universe to send to. The mutex map ensures (DB write + cache update)
// is one critical section per fixture.
std::mutex fixtureUniverseOpMapMutex;
std::unordered_map<fixtureId_t, std::shared_ptr<std::mutex>> fixtureUniverseOpMutexes;

std::shared_ptr<std::mutex> getUniverseOpMutex(const fixtureId_t &fixtureId) {
    std::lock_guard<std::mutex> lock(fixtureUniverseOpMapMutex);
    auto &slot = fixtureUniverseOpMutexes[fixtureId];
    if (!slot)
        slot = std::make_shared<std::mutex>();
    return slot;
}

} // namespace

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

oatpp::Object<ListDto<oatpp::Object<creatures::DmxFixtureDto>>>
DmxFixtureService::getAllFixtures(std::shared_ptr<RequestSpan> parentSpan) {

    if (!creatures::db) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.getAllFixtures", parentSpan)
                    : nullptr;

    auto result = creatures::db->getAllFixtures(span);

    if (!result.isSuccess()) {
        auto err = result.getError().value();
        Status status = Status::CODE_500;
        if (err.getCode() == ServerError::NotFound)
            status = Status::CODE_404;
        else if (err.getCode() == ServerError::InvalidData)
            status = Status::CODE_400;
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        OATPP_ASSERT_HTTP(false, status, err.getMessage().c_str())
    }

    auto fixtures = result.getValue().value();
    auto items = oatpp::Vector<oatpp::Object<creatures::DmxFixtureDto>>::createShared();
    for (const auto &fixture : fixtures) {
        items->emplace_back(creatures::convertToDto(fixture));
    }

    auto page = ListDto<oatpp::Object<creatures::DmxFixtureDto>>::createShared();
    page->count = items->size();
    page->items = items;

    if (span) {
        span->setAttribute("fixtures.count", static_cast<int64_t>(page->count));
        span->setSuccess();
    }

    return page;
}

oatpp::Object<creatures::DmxFixtureDto> DmxFixtureService::getFixture(const oatpp::String &inFixtureId,
                                                                      std::shared_ptr<RequestSpan> parentSpan) {

    if (!creatures::db) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    const std::string fixtureId = inFixtureId ? std::string(inFixtureId) : "";
    if (fixtureId.empty()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "fixtureId is required");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.getFixture", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }

    auto result = creatures::db->getFixture(fixtureId, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        Status status = Status::CODE_500;
        if (err.getCode() == ServerError::NotFound)
            status = Status::CODE_404;
        else if (err.getCode() == ServerError::InvalidData)
            status = Status::CODE_400;
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        OATPP_ASSERT_HTTP(false, status, err.getMessage().c_str())
    }
    if (!result.getValue().has_value()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database returned no fixture value");
    }

    auto fixture = result.getValue().value();
    if (span)
        span->setSuccess();
    return creatures::convertToDto(fixture);
}

oatpp::Object<creatures::DmxFixtureDto> DmxFixtureService::upsertFixture(const std::string &jsonFixture,
                                                                         std::shared_ptr<RequestSpan> parentSpan) {

    if (!creatures::db) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.upsertFixture", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("json.size", static_cast<int64_t>(jsonFixture.length()));
    }

    // Validate before we touch the DB so the user gets a clean 400 instead of cryptic internal errors.
    auto validateSpan =
        creatures::observability ? creatures::observability->createChildOperationSpan("validateJson", span) : nullptr;
    try {
        auto jsonResult = JsonParser::parseJsonString(jsonFixture, "fixture upsert validation", validateSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            if (validateSpan)
                validateSpan->setError(err.getMessage());
            OATPP_ASSERT_HTTP(false, Status::CODE_400, err.getMessage().c_str());
        }
        auto jsonObject = jsonResult.getValue().value();
        auto validation = creatures::db->validateFixtureJson(jsonObject);
        if (!validation.isSuccess()) {
            auto err = validation.getError().value();
            if (validateSpan)
                validateSpan->setError(err.getMessage());
            OATPP_ASSERT_HTTP(false, Status::CODE_400, err.getMessage().c_str());
        }
    } catch (const nlohmann::json::parse_error &e) {
        if (validateSpan)
            validateSpan->recordException(e);
        OATPP_ASSERT_HTTP(false, Status::CODE_400, e.what());
    }
    if (validateSpan)
        validateSpan->setSuccess();

    auto result = creatures::db->upsertFixture(jsonFixture, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        Status status = Status::CODE_500;
        if (err.getCode() == ServerError::InvalidData)
            status = Status::CODE_400;
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        OATPP_ASSERT_HTTP(false, status, err.getMessage().c_str())
    }

    if (!result.getValue().has_value()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database returned no fixture value");
    }

    auto fixture = result.getValue().value();

    // Mirror the persisted universe assignment (if any) into the runtime map.
    if (fixture.assigned_universe.has_value()) {
        creatures::fixtureUniverseMap->put(fixture.id, *fixture.assigned_universe);
    } else {
        creatures::fixtureUniverseMap->remove(fixture.id);
    }

    if (span) {
        span->setAttribute("fixture.id", fixture.id);
        span->setAttribute("fixture.name", fixture.name);
        span->setSuccess();
    }

    return creatures::convertToDto(fixture);
}

void DmxFixtureService::deleteFixture(const oatpp::String &inFixtureId, std::shared_ptr<RequestSpan> parentSpan) {

    if (!creatures::db) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    const std::string fixtureId = inFixtureId ? std::string(inFixtureId) : "";
    if (fixtureId.empty()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "fixtureId is required");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.deleteFixture", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }

    auto result = creatures::db->deleteFixture(fixtureId, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        Status status = Status::CODE_500;
        if (err.getCode() == ServerError::NotFound)
            status = Status::CODE_404;
        else if (err.getCode() == ServerError::InvalidData)
            status = Status::CODE_400;
        if (span) {
            span->setError(err.getMessage());
        }
        OATPP_ASSERT_HTTP(false, status, err.getMessage().c_str())
    }

    creatures::fixtureUniverseMap->remove(fixtureId);

    if (span)
        span->setSuccess();
}

oatpp::Object<creatures::DmxFixtureDto> DmxFixtureService::setFixtureUniverse(const oatpp::String &inFixtureId,
                                                                              std::optional<universe_t> universe,
                                                                              std::shared_ptr<RequestSpan> parentSpan) {

    if (!creatures::db) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Database unavailable");
    }

    const std::string fixtureId = inFixtureId ? std::string(inFixtureId) : "";
    if (fixtureId.empty()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "fixtureId is required");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.setFixtureUniverse", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
        span->setAttribute("fixture.universe.set", universe.has_value());
        if (universe.has_value()) {
            span->setAttribute("fixture.universe", static_cast<int64_t>(*universe));
        }
    }

    // Serialize concurrent (DB write + cache update + re-fetch) ops for this fixture
    // so two racing PUTs can't leave the DB and runtime map disagreeing.
    auto opMutex = getUniverseOpMutex(fixtureId);
    std::lock_guard<std::mutex> opLock(*opMutex);

    auto setResult = creatures::db->setFixtureUniverse(fixtureId, universe, span);
    if (!setResult.isSuccess()) {
        auto err = setResult.getError().value();
        Status status = Status::CODE_500;
        if (err.getCode() == ServerError::NotFound)
            status = Status::CODE_404;
        else if (err.getCode() == ServerError::InvalidData)
            status = Status::CODE_400;
        if (span) {
            span->setError(err.getMessage());
        }
        OATPP_ASSERT_HTTP(false, status, err.getMessage().c_str())
    }

    if (universe.has_value()) {
        creatures::fixtureUniverseMap->put(fixtureId, *universe);
    } else {
        creatures::fixtureUniverseMap->remove(fixtureId);
    }

    // Re-fetch so the response reflects the new state.
    auto fetched = creatures::db->getFixture(fixtureId, span);
    if (!fetched.isSuccess()) {
        auto err = fetched.getError().value();
        if (span) {
            span->setError(err.getMessage());
        }
        OATPP_ASSERT_HTTP(false, Status::CODE_500, err.getMessage().c_str())
    }

    if (span) {
        span->setAttribute("fixture.name", fetched.getValue().value().name);
        span->setSuccess();
    }
    return creatures::convertToDto(fetched.getValue().value());
}

oatpp::Object<FixtureConfigValidationDto>
DmxFixtureService::validateFixtureConfig(const std::string &jsonFixture, std::shared_ptr<RequestSpan> parentSpan) {

    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("DmxFixtureService.validateFixtureConfig", parentSpan)
            : nullptr;

    auto resultDto = FixtureConfigValidationDto::createShared();
    resultDto->valid = true;
    resultDto->missing_creature_ids = oatpp::List<oatpp::String>::createShared();
    resultDto->error_messages = oatpp::List<oatpp::String>::createShared();

    if (!creatures::db) {
        resultDto->valid = false;
        resultDto->error_messages->push_back("Database unavailable");
        if (span)
            span->setError("Database unavailable");
        return resultDto;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(jsonFixture);
    } catch (const std::exception &ex) {
        resultDto->valid = false;
        resultDto->error_messages->push_back(fmt::format("Invalid JSON: {}", ex.what()).c_str());
        if (span)
            span->setError("Invalid JSON");
        return resultDto;
    }

    auto parseResult = creatures::Database::parseFixtureJson(parsed, span);
    if (!parseResult.isSuccess()) {
        resultDto->valid = false;
        resultDto->error_messages->push_back(parseResult.getError()->getMessage().c_str());
        if (span)
            span->setError(parseResult.getError()->getMessage());
        return resultDto;
    }
    if (!parseResult.getValue().has_value()) {
        resultDto->valid = false;
        resultDto->error_messages->push_back("Fixture validation returned no value");
        return resultDto;
    }

    auto fixture = parseResult.getValue().value();
    resultDto->fixture_id = fixture.id.c_str();
    if (span) {
        span->setAttribute("fixture.id", fixture.id);
        span->setAttribute("fixture.bindings_count", static_cast<int64_t>(fixture.bindings.size()));
    }

    // Soft check: warn (don't fail) when bindings reference creatures that don't currently exist.
    // Dedupe up front — a fixture with N bindings to the same creature shouldn't issue N
    // identical Mongo queries (security review H2b).
    std::set<std::string> uniqueCreatureIds;
    for (const auto &binding : fixture.bindings) {
        if (!binding.creature_id.empty()) {
            uniqueCreatureIds.insert(binding.creature_id);
        }
    }
    for (const auto &creatureId : uniqueCreatureIds) {
        auto creatureLookup = creatures::db->getCreature(creatureId, span);
        if (!creatureLookup.isSuccess()) {
            resultDto->missing_creature_ids->push_back(creatureId.c_str());
        }
    }

    if (span) {
        span->setAttribute("validation.passed", static_cast<bool>(resultDto->valid));
        span->setAttribute("validation.missing_creature_ids_count",
                           static_cast<int64_t>(resultDto->missing_creature_ids->size()));
        span->setAttribute("validation.error_count", static_cast<int64_t>(resultDto->error_messages->size()));
        span->setSuccess();
    }
    return resultDto;
}

oatpp::Object<creatures::DmxFixtureDto> DmxFixtureService::triggerPattern(const oatpp::String &inFixtureId,
                                                                          const oatpp::String &inPatternId,
                                                                          std::optional<uint32_t> stopAfterMs,
                                                                          std::shared_ptr<RequestSpan> parentSpan) {

    const std::string fixtureId = inFixtureId ? std::string(inFixtureId) : "";
    const std::string patternId = inPatternId ? std::string(inPatternId) : "";

    if (fixtureId.empty()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "fixtureId is required");
    }
    if (patternId.empty()) {
        OATPP_ASSERT_HTTP(false, Status::CODE_400, "patternId is required");
    }
    if (!creatures::fixturePatternRunner) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Fixture pattern runner unavailable");
    }
    if (!creatures::fixtureCache) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Fixture cache unavailable");
    }
    if (!creatures::fixtureUniverseMap) {
        OATPP_ASSERT_HTTP(false, Status::CODE_500, "Fixture universe map unavailable");
    }

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.triggerPattern", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
        span->setAttribute("pattern.id", patternId);
        if (stopAfterMs.has_value()) {
            span->setAttribute("trigger.stop_after_ms", static_cast<int64_t>(*stopAfterMs));
        }
    }

    // Pull the fixture from cache (falling back to DB).
    std::shared_ptr<DmxFixture> fixture;
    try {
        fixture = creatures::fixtureCache->get(fixtureId);
    } catch (const std::out_of_range &) {
        auto dbResult = creatures::db->getFixture(fixtureId, span);
        if (!dbResult.isSuccess()) {
            if (span)
                span->setError(dbResult.getError()->getMessage());
            OATPP_ASSERT_HTTP(false, Status::CODE_404, dbResult.getError()->getMessage().c_str());
        }
        fixture = std::make_shared<DmxFixture>(dbResult.getValue().value());
        creatures::fixtureCache->put(fixtureId, fixture);
    }

    const FixturePattern *pattern = fixture->findPatternById(patternId);
    if (!pattern) {
        const auto message = fmt::format("Fixture {} has no pattern with id '{}'", fixtureId, patternId);
        if (span)
            span->setError(message);
        OATPP_ASSERT_HTTP(false, Status::CODE_404, message.c_str());
    }

    // Resolve universe via a single locked lookup. The previous contains/get pattern had
    // a TOCTOU window — a concurrent DELETE /universe between the two calls could throw
    // std::out_of_range out of the service.
    const auto universePtr = creatures::fixtureUniverseMap->tryGet(fixtureId);
    if (!universePtr) {
        const auto message =
            fmt::format("Fixture {} has no assigned_universe; assign one before triggering", fixtureId);
        if (span)
            span->setError(message);
        OATPP_ASSERT_HTTP(false, Status::CODE_400, message.c_str());
    }
    const universe_t universe = *universePtr;
    const framenum_t currentFrame = creatures::eventLoop ? creatures::eventLoop->getNextFrameNumber() : 0;

    if (!creatures::fixturePatternRunner->start(*fixture, *pattern, universe, /*creatureId=*/"", currentFrame, span)) {
        const auto message = fmt::format("Failed to start pattern {} on fixture {}", patternId, fixtureId);
        if (span)
            span->setError(message);
        OATPP_ASSERT_HTTP(false, Status::CODE_500, message.c_str());
    }

    // Arm a tick if there isn't already one pending.
    if (creatures::fixturePatternRunner->tryArm() && creatures::eventLoop) {
        auto tickEvent = std::make_shared<FixturePatternTickEvent>(currentFrame);
        creatures::eventLoop->scheduleEvent(tickEvent);
    }

    // Schedule the auto-stop if the caller asked for one. We piggyback on the existing stop()
    // path by scheduling a one-shot lambda event via a small ad-hoc Event subclass below.
    if (stopAfterMs.has_value() && creatures::eventLoop) {
        struct AutoStopEvent : EventBase<AutoStopEvent> {
            std::string fid;
            std::shared_ptr<FixturePatternRunner> runner;
            AutoStopEvent(framenum_t f, std::string id, std::shared_ptr<FixturePatternRunner> r)
                : EventBase(f), fid(std::move(id)), runner(std::move(r)) {}
            Result<framenum_t> executeImpl() {
                if (runner)
                    runner->stop(fid, this->frameNumber);
                return Result<framenum_t>{this->frameNumber};
            }
        };
        const auto stopFrame = currentFrame + static_cast<framenum_t>(*stopAfterMs);
        auto stopEvent = std::make_shared<AutoStopEvent>(stopFrame, fixtureId, creatures::fixturePatternRunner);
        creatures::eventLoop->scheduleEvent(stopEvent);
    }

    if (span)
        span->setSuccess();

    return creatures::convertToDto(*fixture);
}

void DmxFixtureService::hydrateFromDatabase(std::shared_ptr<OperationSpan> parentSpan) {
    if (!creatures::db) {
        warn("DmxFixtureService::hydrateFromDatabase called with no database; skipping");
        return;
    }

    auto span =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("DmxFixtureService.hydrateFromDatabase", parentSpan)
            : nullptr;

    auto result = creatures::db->getAllFixtures(span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        warn("Failed to hydrate fixtures from database: {}", err.getMessage());
        if (span)
            span->setError(err.getMessage());
        return;
    }

    const auto fixtures = result.getValue().value();
    int64_t withUniverse = 0;
    for (const auto &fixture : fixtures) {
        // getAllFixtures already populates fixtureCache. We just need to mirror the universe.
        if (fixture.assigned_universe.has_value()) {
            creatures::fixtureUniverseMap->put(fixture.id, *fixture.assigned_universe);
            ++withUniverse;
        }
    }

    info("Hydrated {} fixtures into cache; {} have assigned universes", fixtures.size(), withUniverse);

    if (span) {
        span->setAttribute("fixtures.count", static_cast<int64_t>(fixtures.size()));
        span->setAttribute("fixtures.with_universe", withUniverse);
        span->setSuccess();
    }
}

} // namespace creatures::ws
