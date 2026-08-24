
#include <algorithm>
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
#include "server/storage/Storage.h"
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
std::unordered_map<fixtureId_t, std::weak_ptr<std::mutex>> fixtureUniverseOpMutexes;

const char *fixtureErrorType(creatures::ServerError::Code code) {
    switch (code) {
    case creatures::ServerError::NotFound:
        return "NotFound";
    case creatures::ServerError::Forbidden:
        return "Forbidden";
    case creatures::ServerError::InvalidData:
        return "InvalidData";
    case creatures::ServerError::DatabaseError:
        return "DatabaseError";
    case creatures::ServerError::Conflict:
        return "Conflict";
    default:
        return "InternalError";
    }
}

template <typename T, typename SpanT>
creatures::Result<T> fixtureServiceError(const std::shared_ptr<SpanT> &span, const creatures::ServerError &error) {
    creatures::recordSpanError(span, error.getMessage(), fixtureErrorType(error.getCode()), error.getCode());
    return creatures::Result<T>{error};
}

std::shared_ptr<std::mutex> getUniverseOpMutex(const fixtureId_t &fixtureId) {
    std::lock_guard<std::mutex> lock(fixtureUniverseOpMapMutex);
    std::erase_if(fixtureUniverseOpMutexes, [](const auto &entry) { return entry.second.expired(); });
    if (const auto existing = fixtureUniverseOpMutexes[fixtureId].lock()) {
        return existing;
    }
    auto mutex = std::make_shared<std::mutex>();
    fixtureUniverseOpMutexes[fixtureId] = mutex;
    return mutex;
}

// One-shot event that fires `stop_after_ms` after a pattern is triggered, transitioning the
// pattern into fade-out. Used by both `triggerPattern` and `previewPattern`.
//
// Trace linkage: this event fires well after the originating request span has ended.
// Capturing trace_id+span_id from the trigger span at schedule time lets the AutoStopEvent
// emit them as `trigger.trace_id` / `trigger.span_id` attributes when it runs, so Honeycomb
// can correlate trigger → auto-stop.
struct AutoStopEvent : creatures::EventBase<AutoStopEvent> {
    fixtureId_t fid;
    uint64_t generation;
    std::shared_ptr<creatures::FixturePatternRunner> runner;
    std::string triggerTraceId;
    std::string triggerSpanId;
    AutoStopEvent(framenum_t f, fixtureId_t id, uint64_t patternGeneration,
                  std::shared_ptr<creatures::FixturePatternRunner> r, std::string traceId, std::string spanId)
        : EventBase(f), fid(std::move(id)), generation(patternGeneration), runner(std::move(r)),
          triggerTraceId(std::move(traceId)), triggerSpanId(std::move(spanId)) {}
    creatures::Result<framenum_t> executeImpl() {
        auto autoStopSpan = creatures::observability
                                ? creatures::observability->createChildOperationSpan(
                                      "FixturePatternRunner.autoStop", std::shared_ptr<creatures::OperationSpan>{})
                                : nullptr;
        if (autoStopSpan) {
            autoStopSpan->setAttribute("fixture.id", fid);
            if (!triggerTraceId.empty()) {
                autoStopSpan->setAttribute("trigger.trace_id", triggerTraceId);
                autoStopSpan->setAttribute("trigger.span_id", triggerSpanId);
            }
        }
        if (runner)
            runner->stopIfGeneration(fid, generation, this->frameNumber, autoStopSpan);
        if (autoStopSpan)
            autoStopSpan->setSuccess();
        return creatures::Result<framenum_t>{this->frameNumber};
    }
};

} // namespace

namespace creatures ::ws {

Result<std::vector<DmxFixture>> DmxFixtureService::getAllFixtures(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.getAllFixtures", parentSpan)
                    : nullptr;

    if (!creatures::db) {
        const ServerError error{ServerError::InternalError, "Database unavailable"};
        if (span)
            span->setError(error.getMessage());
        return Result<std::vector<DmxFixture>>{error};
    }
    auto result = creatures::db->getAllFixtures(span);

    if (!result.isSuccess()) {
        auto err = result.getError().value();
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<std::vector<DmxFixture>>{err};
    }
    const auto fixtures = result.getValue().value();

    if (span) {
        span->setAttribute("fixtures.count", static_cast<int64_t>(fixtures.size()));
        span->setSuccess();
    }

    return Result<std::vector<DmxFixture>>{fixtures};
}

Result<DmxFixture> DmxFixtureService::getFixture(const fixtureId_t &fixtureId,
                                                 std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.getFixture", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }

    if (fixtureId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));

    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));
    auto result = creatures::db->getFixture(fixtureId, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<DmxFixture>{err};
    }
    if (!result.getValue().has_value()) {
        return Result<DmxFixture>{ServerError(ServerError::InternalError, "Database returned no fixture value")};
    }

    auto fixture = result.getValue().value();
    if (span)
        span->setSuccess();
    return Result<DmxFixture>{fixture};
}

Result<DmxFixture> DmxFixtureService::upsertFixture(const std::string &jsonFixture,
                                                    std::shared_ptr<RequestSpan> parentSpan) {

    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.upsertFixture", parentSpan)
                    : nullptr;
    if (span)
        span->setAttribute("json.size", static_cast<int64_t>(jsonFixture.length()));

    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));
    if (!creatures::fixtureUniverseMap)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture universe map unavailable"));

    // Validate before we touch the DB so the user gets a clean 400 instead of cryptic internal errors.
    auto validateSpan =
        creatures::observability ? creatures::observability->createChildOperationSpan("validateJson", span) : nullptr;
    fixtureId_t validatedFixtureId;
    try {
        auto jsonResult = JsonParser::parseApiJsonString(jsonFixture, "fixture upsert validation", validateSpan);
        if (!jsonResult.isSuccess()) {
            auto err = jsonResult.getError().value();
            if (validateSpan)
                validateSpan->setError(err.getMessage());
            return Result<DmxFixture>{err};
        }
        auto jsonObject = jsonResult.getValue().value();
        auto validation = creatures::db->validateFixtureJson(jsonObject);
        if (!validation.isSuccess()) {
            auto err = validation.getError().value();
            if (validateSpan)
                validateSpan->setError(err.getMessage());
            return Result<DmxFixture>{err};
        }
        validatedFixtureId = jsonObject.at("id").get<std::string>();
    } catch (const nlohmann::json::parse_error &e) {
        if (validateSpan)
            validateSpan->recordException(e);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, e.what())};
    }
    if (validateSpan)
        validateSpan->setSuccess();

    auto opMutex = getUniverseOpMutex(validatedFixtureId);
    std::lock_guard<std::mutex> opLock(*opMutex);

    // Storage facade pairs the upsert + Fixture cache invalidation so the
    // controller can't fire the DB call without the broadcast (issue #11).
    auto result = creatures::storage::publishFixture(jsonFixture, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        if (span) {
            span->setError(err.getMessage());
            span->setAttribute("error.code", static_cast<int64_t>(err.getCode()));
        }
        return Result<DmxFixture>{err};
    }

    if (!result.getValue().has_value()) {
        return Result<DmxFixture>{ServerError(ServerError::InternalError, "Database returned no fixture value")};
    }

    auto fixture = result.getValue().value();

    // Mirror the persisted universe assignment (if any) into the runtime map. The DB
    // layer backfills assigned_universe from the existing document when the upsert JSON
    // omits it (preserve semantics, issue #68), so `remove` here only fires when the DB
    // genuinely has no assignment — an upsert can no longer silently strand a fixture
    // with a DB universe but no runtime mapping.
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

    return Result<DmxFixture>{fixture};
}

Result<void> DmxFixtureService::deleteFixture(const fixtureId_t &fixtureId, std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.deleteFixture", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }

    if (fixtureId.empty())
        return fixtureServiceError<void>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));

    if (!creatures::db)
        return fixtureServiceError<void>(span, ServerError(ServerError::InternalError, "Database unavailable"));
    auto opMutex = getUniverseOpMutex(fixtureId);
    std::lock_guard<std::mutex> opLock(*opMutex);
    auto result = creatures::storage::deleteFixture(fixtureId, span);
    if (!result.isSuccess()) {
        auto err = result.getError().value();
        if (span) {
            span->setError(err.getMessage());
        }
        return Result<void>{err};
    }

    if (creatures::fixtureUniverseMap)
        creatures::fixtureUniverseMap->remove(fixtureId);

    if (span)
        span->setSuccess();
    return Result<void>{};
}

Result<DmxFixture> DmxFixtureService::setFixtureUniverse(const fixtureId_t &fixtureId,
                                                         std::optional<universe_t> universe,
                                                         std::shared_ptr<RequestSpan> parentSpan) {
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

    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));
    if (fixtureId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));
    if (!creatures::fixtureUniverseMap)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture universe map unavailable"));

    // Serialize concurrent (DB write + cache update + re-fetch) ops for this fixture
    // so two racing PUTs can't leave the DB and runtime map disagreeing.
    auto opMutex = getUniverseOpMutex(fixtureId);
    std::lock_guard<std::mutex> opLock(*opMutex);

    auto setResult = creatures::storage::setFixtureUniverse(fixtureId, universe, span);
    if (!setResult.isSuccess()) {
        auto err = setResult.getError().value();
        if (span) {
            span->setError(err.getMessage());
        }
        return Result<DmxFixture>{err};
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
        return Result<DmxFixture>{err};
    }
    if (!fetched.getValue().has_value())
        return Result<DmxFixture>{ServerError(ServerError::InternalError, "Database returned no fixture value")};

    if (span) {
        span->setAttribute("fixture.name", fetched.getValue().value().name);
        span->setSuccess();
    }
    return Result<DmxFixture>{fetched.getValue().value()};
}

api::FixtureConfigValidationResponse DmxFixtureService::validateFixtureConfig(const std::string &jsonFixture,
                                                                              std::shared_ptr<RequestSpan> parentSpan) {

    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("DmxFixtureService.validateFixtureConfig", parentSpan)
            : nullptr;

    api::FixtureConfigValidationResponse response;
    response.valid = true;

    if (!creatures::db) {
        response.valid = false;
        response.errorMessages.push_back("Database unavailable");
        if (span)
            span->setError("Database unavailable");
        return response;
    }

    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(jsonFixture);
    } catch (const std::exception &ex) {
        response.valid = false;
        response.errorMessages.push_back(fmt::format("Invalid JSON: {}", ex.what()));
        if (span)
            span->setError("Invalid JSON");
        return response;
    }

    auto parseResult = creatures::Database::parseFixtureJson(parsed, span);
    if (!parseResult.isSuccess()) {
        response.valid = false;
        response.errorMessages.push_back(parseResult.getError()->getMessage());
        if (span)
            span->setError(parseResult.getError()->getMessage());
        return response;
    }
    if (!parseResult.getValue().has_value()) {
        response.valid = false;
        response.errorMessages.push_back("Fixture validation returned no value");
        return response;
    }

    auto fixture = parseResult.getValue().value();
    response.fixtureId = fixture.id;
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
            response.missingCreatureIds.push_back(creatureId);
        }
    }

    if (span) {
        span->setAttribute("validation.passed", response.valid);
        span->setAttribute("validation.missing_creature_ids_count",
                           static_cast<int64_t>(response.missingCreatureIds.size()));
        span->setAttribute("validation.error_count", static_cast<int64_t>(response.errorMessages.size()));
        span->setSuccess();
    }
    return response;
}

Result<DmxFixture> DmxFixtureService::triggerPattern(const fixtureId_t &fixtureId, const std::string &patternId,
                                                     std::optional<uint32_t> stopAfterMs,
                                                     std::shared_ptr<RequestSpan> parentSpan) {
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

    if (fixtureId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));
    if (patternId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "patternId is required"));
    if (!creatures::fixturePatternRunner)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture pattern runner unavailable"));
    if (!creatures::fixtureCache)
        return fixtureServiceError<DmxFixture>(span,
                                               ServerError(ServerError::InternalError, "Fixture cache unavailable"));
    if (!creatures::fixtureUniverseMap)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture universe map unavailable"));
    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));

    // Pull the fixture from cache (falling back to DB).
    std::shared_ptr<DmxFixture> fixture;
    try {
        fixture = creatures::fixtureCache->get(fixtureId);
    } catch (const std::out_of_range &) {
        auto dbResult = creatures::db->getFixture(fixtureId, span);
        if (!dbResult.isSuccess()) {
            if (span)
                span->setError(dbResult.getError()->getMessage());
            return Result<DmxFixture>{dbResult.getError().value()};
        }
        if (!dbResult.getValue().has_value())
            return Result<DmxFixture>{ServerError(ServerError::NotFound, "Fixture not found")};
        fixture = std::make_shared<DmxFixture>(dbResult.getValue().value());
        creatures::fixtureCache->put(fixtureId, fixture);
    }

    const FixturePattern *pattern = fixture->findPatternById(patternId);
    if (!pattern) {
        const auto message = fmt::format("Fixture {} has no pattern with id '{}'", fixtureId, patternId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::NotFound, message)};
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
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
    }
    const universe_t universe = *universePtr;
    const framenum_t currentFrame = creatures::eventLoop ? creatures::eventLoop->getNextFrameNumber() : 0;

    uint64_t patternGeneration = 0;
    if (!creatures::fixturePatternRunner->start(*fixture, *pattern, universe, /*creatureId=*/"", currentFrame, span,
                                                &patternGeneration)) {
        const auto message = fmt::format("Failed to start pattern {} on fixture {}", patternId, fixtureId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::InternalError, message)};
    }

    // Arm a tick if there isn't already one pending.
    if (creatures::fixturePatternRunner->tryArm() && creatures::eventLoop) {
        auto tickEvent = std::make_shared<FixturePatternTickEvent>(currentFrame);
        creatures::eventLoop->scheduleEvent(tickEvent);
    }

    // Schedule the auto-stop if the caller asked for one. AutoStopEvent is shared with
    // previewPattern (see anonymous namespace at top of file).
    if (stopAfterMs.has_value() && creatures::eventLoop) {
        const auto stopFrame = currentFrame + static_cast<framenum_t>(*stopAfterMs);
        // Capture trigger trace context at schedule time — the request span is still live here.
        const std::string traceId = span ? span->getTraceIdHex() : std::string{};
        const std::string spanId = span ? span->getSpanIdHex() : std::string{};
        auto stopEvent = std::make_shared<AutoStopEvent>(stopFrame, fixtureId, patternGeneration,
                                                         creatures::fixturePatternRunner, traceId, spanId);
        creatures::eventLoop->scheduleEvent(stopEvent);
    }

    if (span)
        span->setSuccess();

    return Result<DmxFixture>{*fixture};
}

Result<DmxFixture> DmxFixtureService::previewPattern(const fixtureId_t &fixtureId,
                                                     const std::vector<std::pair<std::string, uint8_t>> &values,
                                                     uint32_t fadeInMs, uint32_t fadeOutMs, uint32_t holdMs,
                                                     std::optional<uint32_t> stopAfterMs,
                                                     std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.previewPattern", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
        span->setAttribute("pattern.preview.value_count", static_cast<int64_t>(values.size()));
        span->setAttribute("pattern.fade_in_ms", static_cast<int64_t>(fadeInMs));
        span->setAttribute("pattern.fade_out_ms", static_cast<int64_t>(fadeOutMs));
        span->setAttribute("pattern.hold_ms", static_cast<int64_t>(holdMs));
        if (stopAfterMs.has_value()) {
            span->setAttribute("trigger.stop_after_ms", static_cast<int64_t>(*stopAfterMs));
        }
    }

    if (fixtureId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));
    if (values.empty())
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InvalidData, "values must contain at least one channel"));
    if (!creatures::fixturePatternRunner)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture pattern runner unavailable"));
    if (!creatures::fixtureCache)
        return fixtureServiceError<DmxFixture>(span,
                                               ServerError(ServerError::InternalError, "Fixture cache unavailable"));
    if (!creatures::fixtureUniverseMap)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture universe map unavailable"));
    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));

    // Resolve the fixture from cache, falling back to DB. Same idiom as triggerPattern.
    std::shared_ptr<DmxFixture> fixture;
    try {
        fixture = creatures::fixtureCache->get(fixtureId);
    } catch (const std::out_of_range &) {
        auto dbResult = creatures::db->getFixture(fixtureId, span);
        if (!dbResult.isSuccess()) {
            if (span)
                span->setError(dbResult.getError()->getMessage());
            return Result<DmxFixture>{dbResult.getError().value()};
        }
        if (!dbResult.getValue().has_value())
            return Result<DmxFixture>{ServerError(ServerError::NotFound, "Fixture not found")};
        fixture = std::make_shared<DmxFixture>(dbResult.getValue().value());
        creatures::fixtureCache->put(fixtureId, fixture);
    }

    // Validate each channel name against the fixture *before* constructing the ephemeral
    // pattern. The runner's start() also checks, but a clean 400 with the offending channel
    // name is friendlier to the editor UI than a generic 500.
    for (const auto &v : values) {
        if (!fixture->findChannelByName(v.first)) {
            const auto message = fmt::format("Fixture {} has no channel named '{}'", fixtureId, v.first);
            if (span) {
                span->setAttribute("error.channel_name", v.first);
                span->setError(message);
            }
            return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
        }
    }

    // Resolve universe via a single locked lookup (same TOCTOU concern as triggerPattern).
    const auto universePtr = creatures::fixtureUniverseMap->tryGet(fixtureId);
    if (!universePtr) {
        const auto message =
            fmt::format("Fixture {} has no assigned_universe; assign one before previewing", fixtureId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
    }
    const universe_t universe = *universePtr;

    if (span)
        span->setAttribute("fixture.universe", static_cast<int64_t>(universe));

    // Build the ephemeral pattern. id/name are present because the struct requires them, but
    // they never get persisted — this object goes straight into the runner's active-pattern
    // map keyed by fixture id, not by pattern id. We tag the id with "preview:" purely so a
    // Honeycomb pivot on `pattern.id` can distinguish preview firings from saved ones.
    FixturePattern ephemeral;
    ephemeral.id = "preview:" + fixtureId;
    ephemeral.name = "preview";
    ephemeral.values.reserve(values.size());
    for (const auto &v : values) {
        FixturePatternValue fpv;
        fpv.channel = v.first;
        fpv.value = v.second;
        ephemeral.values.push_back(fpv);
    }
    ephemeral.fade_in_ms = fadeInMs;
    ephemeral.fade_out_ms = fadeOutMs;
    ephemeral.hold_ms = holdMs;

    const framenum_t currentFrame = creatures::eventLoop ? creatures::eventLoop->getNextFrameNumber() : 0;

    uint64_t patternGeneration = 0;
    if (!creatures::fixturePatternRunner->start(*fixture, ephemeral, universe, /*creatureId=*/"", currentFrame, span,
                                                &patternGeneration)) {
        const auto message = fmt::format("Failed to start preview pattern on fixture {}", fixtureId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
    }

    // Arm a tick if there isn't already one pending.
    if (creatures::fixturePatternRunner->tryArm() && creatures::eventLoop) {
        auto tickEvent = std::make_shared<FixturePatternTickEvent>(currentFrame);
        creatures::eventLoop->scheduleEvent(tickEvent);
    }

    // Schedule the auto-stop if the caller asked for one. Reuses the namespace-scope
    // AutoStopEvent (see top of file).
    if (stopAfterMs.has_value() && creatures::eventLoop) {
        const auto stopFrame = currentFrame + static_cast<framenum_t>(*stopAfterMs);
        const std::string traceId = span ? span->getTraceIdHex() : std::string{};
        const std::string spanId = span ? span->getSpanIdHex() : std::string{};
        auto stopEvent = std::make_shared<AutoStopEvent>(stopFrame, fixtureId, patternGeneration,
                                                         creatures::fixturePatternRunner, traceId, spanId);
        creatures::eventLoop->scheduleEvent(stopEvent);
    }

    if (span)
        span->setSuccess();

    return Result<DmxFixture>{*fixture};
}

Result<DmxFixture> DmxFixtureService::setFixtureLive(const fixtureId_t &fixtureId,
                                                     const std::vector<std::pair<std::string, uint8_t>> &channelValues,
                                                     uint32_t timeoutMs, std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("DmxFixtureService.setFixtureLive", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
        span->setAttribute("fixture.live.channel_value_count", static_cast<int64_t>(channelValues.size()));
        span->setAttribute("fixture.live.timeout_ms", static_cast<int64_t>(timeoutMs));
    }

    if (fixtureId.empty())
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));
    if (channelValues.empty())
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InvalidData, "values must contain at least one channel"));
    if (timeoutMs == 0)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InvalidData, "timeout_ms is required and must be > 0"));
    // Hard cap on the deadline: 10 minutes. Prevents a buggy client from leaving a
    // light driven for hours if the user closes the slider window.
    constexpr uint32_t MAX_LIVE_TIMEOUT_MS = 600'000;
    if (timeoutMs > MAX_LIVE_TIMEOUT_MS) {
        const auto message = fmt::format("timeout_ms exceeds maximum of {}ms", MAX_LIVE_TIMEOUT_MS);
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InvalidData, message));
    }

    if (!creatures::fixturePatternRunner)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture pattern runner unavailable"));
    if (!creatures::fixtureCache)
        return fixtureServiceError<DmxFixture>(span,
                                               ServerError(ServerError::InternalError, "Fixture cache unavailable"));
    if (!creatures::fixtureUniverseMap)
        return fixtureServiceError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Fixture universe map unavailable"));
    if (!creatures::db)
        return fixtureServiceError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));

    // Resolve the fixture (cache → DB fallback).
    std::shared_ptr<DmxFixture> fixture;
    try {
        fixture = creatures::fixtureCache->get(fixtureId);
    } catch (const std::out_of_range &) {
        auto dbResult = creatures::db->getFixture(fixtureId, span);
        if (!dbResult.isSuccess()) {
            if (span)
                span->setError(dbResult.getError()->getMessage());
            return Result<DmxFixture>{dbResult.getError().value()};
        }
        if (!dbResult.getValue().has_value())
            return Result<DmxFixture>{ServerError(ServerError::NotFound, "Fixture not found")};
        fixture = std::make_shared<DmxFixture>(dbResult.getValue().value());
        creatures::fixtureCache->put(fixtureId, fixture);
    }

    // Resolve universe via a single locked lookup (same TOCTOU concern as triggerPattern).
    const auto universePtr = creatures::fixtureUniverseMap->tryGet(fixtureId);
    if (!universePtr) {
        const auto message =
            fmt::format("Fixture {} has no assigned_universe; assign one before live control", fixtureId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
    }
    const universe_t universe = *universePtr;
    const framenum_t currentFrame = creatures::eventLoop ? creatures::eventLoop->getNextFrameNumber() : 0;

    // Surface universe at the service span so Honeycomb pivots like "all live calls to universe N"
    // don't have to descend into the runner child span. Also propagates up to the request span
    // via parent-child attribute inheritance in queries.
    if (span)
        span->setAttribute("fixture.universe", static_cast<int64_t>(universe));

    if (!creatures::fixturePatternRunner->setLive(*fixture, channelValues, timeoutMs, universe, currentFrame, span)) {
        // setLive logs and annotates the span with the specific error; surface 400 because
        // every reason it can fail is caller input (unknown channel name, etc.).
        const auto message = fmt::format("Failed to apply live control to fixture {} (see server logs)", fixtureId);
        if (span)
            span->setError(message);
        return Result<DmxFixture>{ServerError(ServerError::InvalidData, message)};
    }

    // Arm a tick if there isn't already one pending — same pattern as triggerPattern.
    if (creatures::fixturePatternRunner->tryArm() && creatures::eventLoop) {
        auto tickEvent = std::make_shared<FixturePatternTickEvent>(currentFrame);
        creatures::eventLoop->scheduleEvent(tickEvent);
    }

    if (span)
        span->setSuccess();

    return Result<DmxFixture>{*fixture};
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
        if (fixture.assigned_universe.has_value() && creatures::fixtureUniverseMap) {
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
