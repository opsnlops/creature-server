#include <algorithm>
#include <mutex>
#include <optional>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "blockingconcurrentqueue.h"

#include "api/WebSocketEnvelope.h"
#include "exception/exception.h"
#include "model/Creature.h"
#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/config.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/runtime/RuntimeSnapshot.h"
#include "server/storage/Storage.h"
#include "server/ws/service/FixtureActivityHook.h"
#include "util/Result.h"
#include "util/cache.h"

#include "server/runtime/Activity.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h" // Include ObservabilityManager
#include "util/helpers.h"
#include "util/uuidUtils.h"

#include "CreatureService.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability; // Declare observability extern
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern FixtureActivityHook fixtureActivityHook;
} // namespace creatures

namespace creatures ::ws {

namespace {

std::mutex runtimeMutex;
std::unordered_map<std::string, runtime::CreatureRuntimeSnapshot> runtimeState;
std::unordered_map<std::string, std::string> creatureNameCache;
// Highest adoption generation whose activity write was applied, per creature. Guarded by
// runtimeMutex. Versioned writes older than this are dropped — the total-order guard
// that catches late Running writes the session-id heuristic can't (issue #87).
std::unordered_map<std::string, uint64_t> lastActivityGenerationByCreature;
// Track last idle animation per creature to avoid immediate repeats.
std::unordered_map<std::string, std::string> lastIdleAnimationByCreature;

runtime::ActivitySnapshot makeDefaultActivity() {
    auto now = getCurrentTimeISO8601();
    return {"stopped", std::nullopt, std::nullopt, "disabled", now, now};
}

runtime::CreatureRuntimeSnapshot makeDefaultRuntime() {
    runtime::CreatureRuntimeSnapshot snapshot;
    snapshot.activity = makeDefaultActivity();
    return snapshot;
}

runtime::CreatureRuntimeSnapshot &getOrCreateRuntimeLocked(const std::string &creatureId) {
    auto [it, inserted] = runtimeState.try_emplace(creatureId);
    if (inserted)
        it->second = makeDefaultRuntime();
    return it->second;
}

runtime::CreatureRuntimeSnapshot getRuntimeSnapshot(const std::string &creatureId) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    return getOrCreateRuntimeLocked(creatureId);
}

std::optional<std::string> getLastIdleAnimationId(const std::string &creatureId) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    auto it = lastIdleAnimationByCreature.find(creatureId);
    if (it == lastIdleAnimationByCreature.end()) {
        return std::nullopt;
    }
    return it->second;
}

void setLastIdleAnimationId(const std::string &creatureId, const std::string &animationId) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    if (animationId.empty()) {
        lastIdleAnimationByCreature.erase(creatureId);
        return;
    }
    lastIdleAnimationByCreature[creatureId] = animationId;
}

void broadcastIdleStateChanged(const std::string &creatureId, bool enabled) {
    if (!creatures::websocketOutgoingMessages) {
        warn("CreatureService: websocket queue unavailable, skipping idle state broadcast for {}", creatureId);
        return;
    }

    const nlohmann::json payload = {
        {"creature_id", creatureId}, {"idle_enabled", enabled}, {"timestamp", getCurrentTimeISO8601()}};
    creatures::websocketOutgoingMessages->enqueue(
        creatures::api::serializeWebSocketEnvelope(toString(creatures::ws::MessageType::IdleStateChanged), payload));
}

/**
 * Immutable copy of the activity fields a broadcast needs. Taken while runtimeMutex is
 * held so the broadcast itself can run outside the lock (resolveCreatureName takes
 * runtimeMutex, so broadcasting under it would self-deadlock) — issue #81.
 */
void broadcastCreatureActivitySnapshot(const std::string &creatureId, const runtime::ActivitySnapshot &snapshot) {
    if (!creatures::websocketOutgoingMessages) {
        warn("CreatureService: websocket queue unavailable, skipping activity broadcast for {}", creatureId);
        return;
    }
    const nlohmann::json payload = {
        {"creature_id", creatureId},
        {"creature_name", CreatureService::resolveCreatureName(creatureId)},
        {"state", snapshot.state},
        {"animation_id", snapshot.animationId ? nlohmann::json(*snapshot.animationId) : nlohmann::json(nullptr)},
        {"session_id", snapshot.sessionId ? nlohmann::json(*snapshot.sessionId) : nlohmann::json(nullptr)},
        {"reason", snapshot.reason},
        {"timestamp", getCurrentTimeISO8601()}};
    creatures::websocketOutgoingMessages->enqueue(
        creatures::api::serializeWebSocketEnvelope(toString(creatures::ws::MessageType::CreatureActivity), payload));
}

void broadcastCreatureActivity(const std::string &creatureId, const runtime::CreatureRuntimeSnapshot &snapshot) {
    broadcastCreatureActivitySnapshot(creatureId, snapshot.activity);
}

bool animationTargetsSingleCreature(const Animation &animation, const creatureId_t &creatureId) {
    if (animation.tracks.empty()) {
        return false;
    }
    for (const auto &track : animation.tracks) {
        if (track.creature_id != creatureId) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> shuffledIds(const std::vector<std::string> &ids) {
    std::vector<std::string> shuffled = ids;
    if (shuffled.size() <= 1) {
        return shuffled;
    }
    std::random_device rd;
    std::mt19937 gen(rd());
    std::shuffle(shuffled.begin(), shuffled.end(), gen);
    return shuffled;
}

} // namespace

std::string CreatureService::resolveCreatureName(const creatureId_t &creatureId) {
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        auto it = creatureNameCache.find(creatureId);
        if (it != creatureNameCache.end()) {
            return it->second;
        }
    }

    if (!creatures::db) {
        warn("CreatureService: database unavailable while resolving creature name for {}, using ID fallback",
             creatureId);
        return creatureId;
    }

    auto creatureResult = creatures::db->getCreature(creatureId, nullptr);
    if (!creatureResult.isSuccess()) {
        warn("CreatureService: failed to resolve creature name for {}: {}", creatureId,
             creatureResult.getError()->getMessage());
        return creatureId;
    }

    auto creature = creatureResult.getValue().value();
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        creatureNameCache.emplace(creatureId, creature.name);
    }
    return creature.name;
}

// The live-streaming registry: which creatures are owned by a live console stream right
// now. This is deliberately separate from the runtime activity state — activity writes are
// broadcast state and can transiently disagree (a timeout writing "stopped" a beat before
// the next frame lands), while the registry is set on the first frame and cleared only
// when streaming genuinely ends, so schedulers can trust it (issue #74).
namespace {
std::mutex streamingRegistryMutex;
std::unordered_set<creatureId_t> streamingRegistry;
} // namespace

bool CreatureService::markStreamingIfNew(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingRegistryMutex);
    auto [it, inserted] = streamingRegistry.insert(creatureId);
    return inserted;
}

void CreatureService::clearStreaming(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingRegistryMutex);
    streamingRegistry.erase(creatureId);
}

bool CreatureService::isCreatureStreaming(const creatureId_t &creatureId) {
    {
        std::lock_guard<std::mutex> lock(streamingRegistryMutex);
        if (streamingRegistry.contains(creatureId)) {
            return true;
        }
    }

    // Second signal: the runtime activity state. Kept so anything that marks a creature
    // streaming via setActivityState alone still counts — the registry above protects
    // the windows where the two disagree.
    const auto snapshot = getRuntimeSnapshot(creatureId);
    return snapshot.activity.reason == creatures::runtime::toString(creatures::runtime::ActivityReason::Streaming) &&
           snapshot.activity.state == creatures::runtime::toString(creatures::runtime::ActivityState::Running);
}

std::vector<std::pair<std::string, runtime::CreatureRuntimeSnapshot>> CreatureService::getRuntimeStates() {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    std::vector<std::pair<std::string, runtime::CreatureRuntimeSnapshot>> snapshot;
    snapshot.reserve(runtimeState.size());
    for (const auto &entry : runtimeState) {
        snapshot.emplace_back(entry.first, entry.second);
    }
    return snapshot;
}

Result<std::vector<api::CreatureResponse>> CreatureService::getAllCreatures(std::shared_ptr<RequestSpan> parentSpan) {
    auto logger = spdlog::default_logger();
    if (!parentSpan) {
        warn("no parent span provided for CreatureService.getAllCreatures, creating a root span");
    }
    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("CreatureService.getAllCreatures", std::move(parentSpan))
            : nullptr;
    if (span) {
        span->setAttribute("service", "CreatureService");
        span->setAttribute("operation", "getAllCreatures");
    }
    if (!db) {
        const ServerError error{ServerError::InternalError, "Creature database unavailable"};
        recordSpanError(span, error.getMessage(), "InternalError", error.getCode());
        return Result<std::vector<api::CreatureResponse>>{error};
    }
    if (logger)
        logger->debug("CreatureService::getAllCreatures()");

    auto result = db->getAllCreatures(creatures::SortBy::name, true, span);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        if (logger)
            logger->warn("{}", error.getMessage());
        recordSpanError(span, error.getMessage(), "CreatureLookupFailed", error.getCode());
        return Result<std::vector<api::CreatureResponse>>{error};
    }

    std::vector<api::CreatureResponse> responses;
    const auto creatures = result.getValue().value();
    responses.reserve(creatures.size());
    for (const auto &creature : creatures) {
        if (logger)
            logger->debug("Adding creature: {}", creature.id);
        responses.push_back({creature, getRuntimeSnapshot(creature.id)});
    }
    if (span) {
        span->setAttribute("creatures.count", static_cast<int64_t>(responses.size()));
        span->setSuccess();
    }
    return Result<std::vector<api::CreatureResponse>>{responses};
}

Result<api::CreatureResponse> CreatureService::getCreature(const creatureId_t &creatureId,
                                                           std::shared_ptr<RequestSpan> parentSpan) {
    auto logger = spdlog::default_logger();
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("CreatureService.getCreature", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("service", "CreatureService");
        span->setAttribute("operation", "getCreature");
        span->setAttribute("creature.id", creatureId);
    }
    if (!db) {
        const ServerError error{ServerError::InternalError, "Creature database unavailable"};
        recordSpanError(span, error.getMessage(), "InternalError", error.getCode());
        return Result<api::CreatureResponse>{error};
    }
    if (logger)
        logger->debug("CreatureService::getCreature({})", creatureId);

    auto result = db->getCreature(creatureId, span);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        if (logger)
            logger->warn("{}", error.getMessage());
        recordSpanError(span, error.getMessage(), "CreatureLookupFailed", error.getCode());
        return Result<api::CreatureResponse>{error};
    }
    const auto creature = result.getValue().value();
    if (span) {
        span->setAttribute("creature.name", creature.name);
        span->setSuccess();
    }
    return Result<api::CreatureResponse>{{creature, getRuntimeSnapshot(creature.id)}};
}

Result<api::CreatureResponse> CreatureService::upsertCreature(const std::string &jsonCreature,
                                                              std::shared_ptr<RequestSpan> parentSpan,
                                                              std::shared_ptr<OperationSpan> parentOperationSpan) {
    auto logger = spdlog::default_logger();
    auto serviceSpan =
        creatures::observability
            ? (parentOperationSpan
                   ? creatures::observability->createChildOperationSpan("CreatureService.upsertCreature",
                                                                        parentOperationSpan)
                   : creatures::observability->createOperationSpan("CreatureService.upsertCreature", parentSpan))
            : nullptr;
    if (logger)
        logger->info("attempting to upsert a creature");
    if (serviceSpan) {
        serviceSpan->setAttribute("service", "CreatureService");
        serviceSpan->setAttribute("operation", "upsertCreature");
        serviceSpan->setAttribute("json.size", static_cast<int64_t>(jsonCreature.length()));
    }

    if (!db) {
        const ServerError error{ServerError::InternalError, "Creature database unavailable"};
        recordSpanError(serviceSpan, error.getMessage(), "InternalError", error.getCode());
        return Result<api::CreatureResponse>{error};
    }

    // ✨ Create a span for the validation step in the service
    auto validationSpan =
        creatures::observability
            ? creatures::observability->createChildOperationSpan("CreatureService.validateJson", serviceSpan)
            : nullptr;
    try {
        auto jsonResult = JsonParser::parseApiJsonString(jsonCreature, "creature upsert validation", validationSpan);
        if (!jsonResult.isSuccess()) {
            const auto parseError = jsonResult.getError().value();
            if (logger)
                logger->warn("{}", parseError.getMessage());
            recordSpanError(serviceSpan, parseError.getMessage(), "JSONParseError", parseError.getCode());
            return Result<api::CreatureResponse>{parseError};
        }
        auto jsonObject = jsonResult.getValue().value();
        auto result = db->validateCreatureJson(jsonObject);
        if (validationSpan)
            validationSpan->setAttribute("validator", "validateCreatureJson");

        if (!result.isSuccess()) {
            const auto error = result.getError().value();
            if (logger)
                logger->warn("{}", error.getMessage());
            recordSpanError(validationSpan, error.getMessage(), "InvalidData", error.getCode());
            recordSpanError(serviceSpan, error.getMessage(), "InvalidData", error.getCode());
            return Result<api::CreatureResponse>{ServerError{ServerError::InvalidData, error.getMessage()}};
        }
        if (validationSpan)
            validationSpan->setSuccess();
    } catch (const nlohmann::json::parse_error &e) {
        if (logger)
            logger->warn("{}", e.what());
        if (validationSpan)
            validationSpan->recordException(e);
        if (serviceSpan)
            serviceSpan->recordException(e);
        recordSpanError(serviceSpan, e.what(), "JSONParseError", ServerError::InvalidData);
        return Result<api::CreatureResponse>{ServerError{ServerError::InvalidData, e.what()}};
    }

    if (logger)
        logger->debug("passing the upsert request off to the storage facade");
    // Creature configs are hand-authored controller documents. Pass the
    // original JSON to storage so unmodeled hardware fields survive; never
    // rebuild the persisted document from the parsed Creature model.
    // The facade also pairs the upsert + Creature cache invalidation atomically.
    auto result = creatures::storage::publishCreature(jsonCreature, serviceSpan);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        if (logger)
            logger->warn("{}", error.getMessage());
        recordSpanError(serviceSpan, error.getMessage(), "DatabaseError", error.getCode());
        return Result<api::CreatureResponse>{error};
    }
    if (!result.getValue().has_value()) {
        const ServerError error{ServerError::InternalError,
                                "DB didn't return a value after upserting the creature. This is a bug. Please report "
                                "it."};
        if (logger)
            logger->error("{}", error.getMessage());
        recordSpanError(serviceSpan, error.getMessage(), "MissingDatabaseResult", error.getCode());
        return Result<api::CreatureResponse>{error};
    }

    const auto creature = result.getValue().value();
    info("Updated {} in the database", creature.name);
    if (serviceSpan) {
        serviceSpan->setAttribute("creature.id", creature.id);
        serviceSpan->setAttribute("creature.name", creature.name);
        serviceSpan->setSuccess();
    }
    return Result<api::CreatureResponse>{api::CreatureResponse{creature, getRuntimeSnapshot(creature.id)}};
}

Result<api::CreatureResponse> CreatureService::registerCreature(const std::string &jsonCreature, universe_t universe,
                                                                std::shared_ptr<RequestSpan> parentSpan) {
    auto logger = spdlog::default_logger();
    if (!parentSpan) {
        warn("no parent span provided for CreatureService.registerCreature, creating a root span");
    }
    auto serviceSpan =
        creatures::observability
            ? creatures::observability->createOperationSpan("CreatureService.registerCreature", parentSpan)
            : nullptr;
    if (logger)
        logger->info("Controller registering creature with universe {}", universe);
    if (serviceSpan) {
        serviceSpan->setAttribute("service", "CreatureService");
        serviceSpan->setAttribute("operation", "registerCreature");
        serviceSpan->setAttribute("universe", static_cast<int64_t>(universe));
        serviceSpan->setAttribute("json.size", static_cast<int64_t>(jsonCreature.length()));
    }

    if (!creatureUniverseMap) {
        const ServerError error{ServerError::InternalError, "Creature universe map unavailable"};
        recordSpanError(serviceSpan, error.getMessage(), "InternalError", error.getCode());
        return Result<api::CreatureResponse>{error};
    }

    if (universe < 1 || universe > 63999) {
        constexpr auto message = "universe must be in [1, 63999]";
        recordSpanError(serviceSpan, message, "InvalidUniverse", ServerError::InvalidData);
        return Result<api::CreatureResponse>{ServerError{ServerError::InvalidData, message}};
    }

    auto creatureResult = upsertCreature(jsonCreature, nullptr, serviceSpan);
    if (!creatureResult.isSuccess()) {
        const auto error = creatureResult.getError().value();
        recordSpanError(serviceSpan, error.getMessage(), "CreatureUpsertFailed", error.getCode());
        return Result<api::CreatureResponse>{error};
    }
    auto response = creatureResult.getValue().value();

    const std::string creatureId = response.creature.id;
    creatures::creatureUniverseMap->put(creatureId, universe);
    if (logger)
        logger->info("Registered creature '{}' (id: {}) on universe {}", response.creature.name, creatureId, universe);

    // Default to idle disabled on registration; clients must explicitly enable it.
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        auto &runtime = getOrCreateRuntimeLocked(creatureId);
        runtime.idleEnabled = false;
        runtime.activity.animationId.reset();
        runtime.activity.sessionId.reset();
        runtime.activity.reason = creatures::runtime::toString(creatures::runtime::ActivityReason::Disabled);
        runtime.activity.state = creatures::runtime::toString(creatures::runtime::ActivityState::Disabled);
        const auto now = getCurrentTimeISO8601();
        runtime.activity.startedAt = now;
        runtime.activity.updatedAt = now;
        response.runtime = runtime;
    }
    broadcastCreatureActivity(creatureId, response.runtime);

    if (serviceSpan) {
        serviceSpan->setAttribute("creature.id", creatureId);
        serviceSpan->setAttribute("creature.name", response.creature.name);
        serviceSpan->setSuccess();
    }
    return Result<api::CreatureResponse>{response};
}

Result<api::CreatureResponse> CreatureService::setIdleEnabled(const creatureId_t &creatureId, bool enabled,
                                                              std::shared_ptr<RequestSpan> parentSpan) {
    auto logger = spdlog::default_logger();
    if (logger)
        logger->info("Setting idle {} for creature {}", enabled ? "enabled" : "disabled", creatureId);
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("CreatureService.setIdleEnabled", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("creature.id", creatureId);
        span->setAttribute("idle.enabled", enabled);
    }

    if (!db) {
        const ServerError error{ServerError::InternalError, "Creature database unavailable"};
        recordSpanError(span, error.getMessage(), "InternalError", error.getCode());
        return Result<api::CreatureResponse>{error};
    }

    // Ensure creature exists
    auto creatureResult = db->getCreature(creatureId, span);
    if (!creatureResult.isSuccess()) {
        const auto error = creatureResult.getError().value();
        recordSpanError(span, error.getMessage(), "CreatureLookupFailed", error.getCode());
        return Result<api::CreatureResponse>{error};
    }

    // Update runtime state early so cancellations won't restart idle.
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        getOrCreateRuntimeLocked(creatureId).idleEnabled = enabled;
    }

    bool cancelledIdleSession = false;
    if (!enabled && creatures::creatureUniverseMap && creatures::creatureUniverseMap->contains(creatureId) &&
        creatures::sessionManager) {
        auto universePtr = creatures::creatureUniverseMap->get(creatureId);
        if (universePtr) {
            cancelledIdleSession = creatures::sessionManager->cancelIdleSessionForCreature(*universePtr, creatureId);
        }
    }

    runtime::CreatureRuntimeSnapshot runtimeSnapshot;
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        auto &runtime = getOrCreateRuntimeLocked(creatureId);
        runtime.activity.animationId.reset();
        runtime.activity.sessionId.reset();
        runtime.activity.reason = creatures::runtime::toString(enabled ? creatures::runtime::ActivityReason::Idle
                                                                       : creatures::runtime::ActivityReason::Disabled);
        runtime.activity.state = creatures::runtime::toString(enabled ? creatures::runtime::ActivityState::Idle
                                                                      : creatures::runtime::ActivityState::Disabled);
        const auto now = getCurrentTimeISO8601();
        runtime.activity.startedAt = now;
        runtime.activity.updatedAt = now;
        ++runtime.counters.idleTogglesTotal;
        if (!enabled && !cancelledIdleSession) {
            ++runtime.counters.idleStoppedTotal;
        }
        runtimeSnapshot = runtime;
    }

    broadcastIdleStateChanged(creatureId, enabled);
    broadcastCreatureActivity(creatureId, runtimeSnapshot);

    if (enabled) {
        startIdleIfNeeded(creatureId, span);
        runtimeSnapshot = getRuntimeSnapshot(creatureId);
    }

    if (span) {
        span->setAttribute("creature.name", creatureResult.getValue()->name);
        span->setSuccess();
    }
    return Result<api::CreatureResponse>{
        api::CreatureResponse{creatureResult.getValue().value(), std::move(runtimeSnapshot)}};
}

// Helper to decide idle vs disabled state when attempting to mark idle
static creatures::runtime::ActivityState resolveIdleState(const runtime::CreatureRuntimeSnapshot &runtime,
                                                          creatures::runtime::ActivityState requested) {
    if (requested == creatures::runtime::ActivityState::Idle && !runtime.idleEnabled) {
        return creatures::runtime::ActivityState::Disabled;
    }
    return requested;
}

bool CreatureService::isStaleActivityWrite(creatures::runtime::ActivityState requestedState,
                                           const std::string &incomingSessionId,
                                           const std::optional<std::string> &currentSessionId) {
    if (requestedState == creatures::runtime::ActivityState::Running) {
        return false;
    }
    if (incomingSessionId.empty() || !currentSessionId.has_value()) {
        return false;
    }
    return *currentSessionId != incomingSessionId;
}

std::string CreatureService::setActivityState(const std::vector<creatureId_t> &creatureIds,
                                              const std::string &animationId, runtime::ActivityReason reason,
                                              runtime::ActivityState state, const std::string &sessionId,
                                              std::shared_ptr<OperationSpan> parentSpan, uint64_t activityGeneration) {

    // Generate or reuse session ID
    std::string sid = sessionId.empty() ? creatures::util::generateUUID() : sessionId;
    auto now = getCurrentTimeISO8601();

    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("CreatureService.setActivityState", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("activity.reason", creatures::runtime::toString(reason));
        span->setAttribute("activity.state", creatures::runtime::toString(state));
        span->setAttribute("session.id", sid);
        span->setAttribute("animation.id", animationId);
        span->setAttribute("creatures.count", static_cast<int64_t>(creatureIds.size()));
        span->setAttribute("activity.generation", static_cast<int64_t>(activityGeneration));
    }

    int64_t writesApplied = 0;
    int64_t staleWritesIgnored = 0;

    for (const auto &creatureId : creatureIds) {
        if (creatureId.empty()) {
            continue;
        }
        bool stale = false;
        std::string staleReason;
        std::string ownerSession;
        auto resolvedState = state;
        auto resolvedReason = reason;
        runtime::ActivitySnapshot snapshot;

        {
            // Hold runtimeMutex across the read-check-mutate: the event-loop runner,
            // REST adoption threads, and the async audio-load worker can all write the
            // same creature's activity concurrently (issue #81). The broadcast runs
            // from the snapshot after the lock is released — resolveCreatureName takes
            // runtimeMutex itself, so it can't be called from in here.
            std::lock_guard<std::mutex> lock(runtimeMutex);
            auto &runtime = getOrCreateRuntimeLocked(creatureId);

            // Ownership guard: a dying session's stop/cancel must not clobber the state
            // of a newer session that already took this creature over (issue #62).
            const auto currentSession = runtime.activity.sessionId;
            uint64_t &lastGeneration = lastActivityGenerationByCreature[creatureId];
            if (activityGeneration != 0 && activityGeneration < lastGeneration) {
                // Generation guard: a versioned write from an older adoption — even a
                // Running write, which the session-id heuristic always lets through —
                // must not clobber the state of a newer adoption (issue #87).
                stale = true;
                staleReason = "older_generation";
            } else if (isStaleActivityWrite(state, sessionId, currentSession)) {
                stale = true;
                staleReason = "session_ownership";
                ownerSession = *currentSession;
            } else {
                resolvedState = resolveIdleState(runtime, state);
                if (resolvedState == creatures::runtime::ActivityState::Disabled &&
                    state == creatures::runtime::ActivityState::Idle) {
                    resolvedReason = creatures::runtime::ActivityReason::Disabled;
                }

                runtime.activity.state = creatures::runtime::toString(resolvedState);
                if (resolvedState == creatures::runtime::ActivityState::Running) {
                    runtime.activity.animationId = animationId;
                    runtime.activity.sessionId = sid;
                } else {
                    runtime.activity.animationId.reset();
                    runtime.activity.sessionId.reset();
                }
                runtime.activity.reason = creatures::runtime::toString(resolvedReason);
                runtime.activity.startedAt = now;
                runtime.activity.updatedAt = now;

                if (resolvedState == creatures::runtime::ActivityState::Running)
                    ++runtime.counters.sessionsStartedTotal;
                if (resolvedReason == creatures::runtime::ActivityReason::Cancelled)
                    ++runtime.counters.sessionsCancelledTotal;

                if (activityGeneration > lastGeneration) {
                    lastGeneration = activityGeneration;
                }

                snapshot = runtime.activity;
            }
        }

        if (stale) {
            debug("Ignoring stale activity write for creature {} from session {} ({})", creatureId, sessionId,
                  staleReason);
            ++staleWritesIgnored;
            if (span) {
                // Record the loser, the reason, and (for ownership drops) the owner so
                // Honeycomb can show exactly which session's write got dropped and why.
                span->setAttribute("activity.stale_write.creature_id", creatureId);
                span->setAttribute("activity.stale_write.reason", staleReason);
                if (!ownerSession.empty()) {
                    span->setAttribute("activity.stale_write.owner_session_id", ownerSession);
                }
            }
            continue;
        }
        ++writesApplied;

        broadcastCreatureActivitySnapshot(creatureId, snapshot);

        // Notify fixture bindings of the (possibly new) activity state. The hook is installed
        // by main.cpp at startup; in tests it stays empty so no fixture work runs.
        if (creatures::fixtureActivityHook) {
            creatures::fixtureActivityHook(creatureId, resolvedReason, resolvedState, span);
        }
    }

    if (span) {
        span->setAttribute("activity.writes_applied", writesApplied);
        span->setAttribute("activity.stale_writes_ignored", staleWritesIgnored);
        span->setSuccess();
    }

    return sid;
}

std::string CreatureService::setActivityRunning(const std::vector<creatureId_t> &creatureIds,
                                                const std::string &animationId, runtime::ActivityReason reason,
                                                const std::string &sessionId, std::shared_ptr<OperationSpan> parentSpan,
                                                uint64_t activityGeneration) {
    return setActivityState(creatureIds, animationId, reason, creatures::runtime::ActivityState::Running, sessionId,
                            parentSpan, activityGeneration);
}

void CreatureService::incrementIdleStopped(const std::vector<creatureId_t> &creatureIds) {
    for (const auto &creatureId : creatureIds) {
        if (creatureId.empty()) {
            continue;
        }
        std::lock_guard<std::mutex> lock(runtimeMutex);
        ++getOrCreateRuntimeLocked(creatureId).counters.idleStoppedTotal;
    }
}

bool CreatureService::startIdleIfNeeded(const creatureId_t &creatureId, std::shared_ptr<OperationSpan> parentSpan) {
    if (creatureId.empty()) {
        return false;
    }

    if (!creatures::db || !creatures::eventLoop || !creatures::sessionManager || !creatures::creatureUniverseMap) {
        warn("CreatureService: idle scheduling unavailable (missing dependencies)");
        return false;
    }

    if (!getRuntimeSnapshot(creatureId).idleEnabled) {
        return false;
    }

    if (CreatureService::isCreatureStreaming(creatureId)) {
        return false;
    }

    if (!creatures::creatureUniverseMap->contains(creatureId)) {
        return false;
    }

    std::shared_ptr<universe_t> universePtr;
    try {
        universePtr = creatures::creatureUniverseMap->get(creatureId);
    } catch (const std::out_of_range &) {
        return false;
    }
    if (!universePtr) {
        return false;
    }
    universe_t universe = *universePtr;

    if (creatures::sessionManager->hasActiveSessionForCreature(universe, creatureId)) {
        return false;
    }

    Creature creature;
    bool creatureLoaded = false;
    if (creatures::creatureCache && creatures::creatureCache->contains(creatureId)) {
        try {
            creature = *creatures::creatureCache->get(creatureId);
            creatureLoaded = true;
        } catch (const std::out_of_range &) {
            creatureLoaded = false;
        }
    }
    if (!creatureLoaded) {
        auto creatureResult = creatures::db->getCreature(creatureId, parentSpan);
        if (!creatureResult.isSuccess() || !creatureResult.getValue().has_value()) {
            return false;
        }
        creature = creatureResult.getValue().value();
        creatureLoaded = true;
    }

    if (!creatureLoaded) {
        return false;
    }
    if (creature.idle_animation_ids.empty()) {
        return false;
    }

    auto candidates = shuffledIds(creature.idle_animation_ids);
    // Push the last-used idle to the end so we try other options first.
    auto lastIdleAnimation = getLastIdleAnimationId(creatureId);
    if (lastIdleAnimation && candidates.size() > 1) {
        candidates.erase(std::remove(candidates.begin(), candidates.end(), *lastIdleAnimation), candidates.end());
        candidates.push_back(*lastIdleAnimation);
    }
    for (const auto &animationId : candidates) {
        if (animationId.empty()) {
            continue;
        }
        auto animationResult = creatures::db->getAnimation(animationId, parentSpan);
        if (!animationResult.isSuccess() || !animationResult.getValue().has_value()) {
            warn("CreatureService: idle animation {} not found for creature {}", animationId, creatureId);
            continue;
        }

        auto animation = animationResult.getValue().value();
        if (!animationTargetsSingleCreature(animation, creatureId)) {
            warn("CreatureService: idle animation {} targets multiple creatures; skipping for {}", animationId,
                 creatureId);
            continue;
        }

        framenum_t startFrame = creatures::eventLoop->getNextFrameNumber() + ANIMATION_DELAY_FRAMES;
        auto scheduleResult =
            scheduleAnimation(startFrame, animation, universe, creatures::runtime::ActivityReason::Idle);
        if (!scheduleResult.isSuccess()) {
            warn("CreatureService: failed to schedule idle animation {} for {}: {}", animationId, creatureId,
                 scheduleResult.getError()->getMessage());
            return false;
        }

        setLastIdleAnimationId(creatureId, animationId);

        {
            std::lock_guard<std::mutex> lock(runtimeMutex);
            ++getOrCreateRuntimeLocked(creatureId).counters.idleStartedTotal;
        }

        return true;
    }

    return false;
}

api::CreatureConfigValidationResponse CreatureService::validateCreatureConfig(const std::string &jsonCreature,
                                                                              std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        creatures::observability
            ? creatures::observability->createOperationSpan("CreatureService.validateCreatureConfig", parentSpan)
            : nullptr;
    api::CreatureConfigValidationResponse response;

    if (!creatures::db) {
        response.valid = false;
        response.errorMessages.emplace_back("Database unavailable");
        if (span) {
            span->setError("Database unavailable");
        }
        return response;
    }

    const auto parsedResult = JsonParser::parseApiJsonString(jsonCreature, "creature validation", span);
    if (!parsedResult.isSuccess()) {
        response.valid = false;
        const auto message = parsedResult.getError()->getMessage();
        response.errorMessages.push_back(message);
        recordSpanError(span, message, "JSONParseError", parsedResult.getError()->getCode());
        return response;
    }
    const auto parsed = parsedResult.getValue().value();

    auto creatureResult = creatures::db->parseCreatureJson(parsed, span);
    if (!creatureResult.isSuccess()) {
        response.valid = false;
        response.errorMessages.push_back(creatureResult.getError()->getMessage());
        recordSpanError(span, creatureResult.getError()->getMessage(), "InvalidData",
                        creatureResult.getError()->getCode());
        return response;
    }
    if (!creatureResult.getValue().has_value()) {
        response.valid = false;
        response.errorMessages.emplace_back("Creature validation returned no value");
        if (span) {
            span->setError("Creature validation returned no value");
        }
        return response;
    }

    auto creature = creatureResult.getValue().value();
    response.creatureId = creature.id;

    // Degree-of-freedom sanity: the mouth reference has to actually point at
    // the beak (issue #120). The slot NUMBER is meaningless on its own and
    // differs per creature; what matters is the link. A mismatch means the
    // viseme stream is driving whatever axis happens to sit at that slot —
    // body_lean, head_height — instead of the beak, so the bird's mouth never
    // moves and something else twitches in time with the words.
    if (const auto matches = creatures::mouthSlotMatchesBeak(creature); matches.has_value() && !*matches) {
        const auto beakSlot = creatures::inputSlotByName(creature, "beak");
        const auto effectiveSlot = creatures::resolvedMouthSlot(creature);
        std::string atThatSlot = "nothing";
        for (const auto &input : creature.inputs) {
            if (input.slot == effectiveSlot) {
                atThatSlot = input.name;
                break;
            }
        }
        auto message = fmt::format(
            "mouth points at slot {} ('{}') but this creature's beak is at slot {}. Lip sync will drive '{}' "
            "instead of the beak. Set \"mouth_input\": \"beak\", or correct mouth_slot to {}.",
            effectiveSlot, atThatSlot, *beakSlot, atThatSlot, *beakSlot);
        warn("Creature '{}': {}", creature.name, message);
        response.valid = false;
        response.errorMessages.push_back(message);
        if (span) {
            span->setAttribute("creature.mouth_slot_matches_beak", false);
        }
    }

    auto checkAnimations = [&](const std::vector<std::string> &ids) {
        for (const auto &animationId : ids) {
            if (animationId.empty()) {
                response.valid = false;
                response.errorMessages.emplace_back("Animation ID cannot be empty");
                continue;
            }
            auto animationResult = creatures::db->getAnimation(animationId, span);
            if (!animationResult.isSuccess() || !animationResult.getValue().has_value()) {
                response.valid = false;
                response.missingAnimationIds.push_back(animationId);
                continue;
            }
            auto animation = animationResult.getValue().value();
            if (!animationTargetsSingleCreature(animation, creature.id)) {
                response.valid = false;
                response.mismatchedAnimationIds.push_back(animationId);
            }
        }
    };

    checkAnimations(creature.idle_animation_ids);
    checkAnimations(creature.speech_loop_animation_ids);

    if (span) {
        span->setAttribute("creature.validation.missing_animation_count",
                           static_cast<int64_t>(response.missingAnimationIds.size()));
        span->setAttribute("creature.validation.mismatched_animation_count",
                           static_cast<int64_t>(response.mismatchedAnimationIds.size()));
        span->setAttribute("creature.validation.error_count", static_cast<int64_t>(response.errorMessages.size()));
        if (response.valid) {
            span->setSuccess();
        } else {
            span->setError("Creature config validation failed");
        }
    }

    return response;
}

} // namespace creatures::ws
