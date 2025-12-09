#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include <mutex>
#include <unordered_map>

#include "blockingconcurrentqueue.h"

#include "exception/exception.h"
#include "model/Creature.h"
#include "server/database.h"
#include "util/Result.h"
#include "util/cache.h"

#include "server/runtime/Activity.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/websocket/CreatureActivityMessage.h"
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
} // namespace creatures

namespace creatures ::ws {

using oatpp::web::protocol::http::Status;

namespace {

std::mutex runtimeMutex;
std::unordered_map<std::string, oatpp::Object<creatures::CreatureRuntimeDto>> runtimeState;
std::unordered_map<std::string, std::string> creatureNameCache;

oatpp::Object<creatures::CreatureRuntimeCountersDto> makeDefaultCounters() {
    auto counters = creatures::CreatureRuntimeCountersDto::createShared();
    counters->sessions_started_total = static_cast<v_uint64>(0);
    counters->sessions_cancelled_total = static_cast<v_uint64>(0);
    counters->idle_started_total = static_cast<v_uint64>(0);
    counters->idle_stopped_total = static_cast<v_uint64>(0);
    counters->idle_toggles_total = static_cast<v_uint64>(0);
    counters->skips_missing_creature_total = static_cast<v_uint64>(0);
    counters->bgm_takeovers_total = static_cast<v_uint64>(0);
    counters->audio_resets_total = static_cast<v_uint64>(0);
    return counters;
}

oatpp::Object<creatures::CreatureRuntimeActivityDto> makeDefaultActivity() {
    auto activity = creatures::CreatureRuntimeActivityDto::createShared();
    activity->state = "stopped";
    activity->animation_id = nullptr;
    activity->session_id = nullptr;
    activity->reason = "disabled";
    auto now = getCurrentTimeISO8601();
    activity->started_at = now;
    activity->updated_at = now;
    return activity;
}

oatpp::Object<creatures::CreatureRuntimeDto> makeDefaultRuntime() {
    auto runtime = creatures::CreatureRuntimeDto::createShared();
    runtime->idle_enabled = true;
    runtime->activity = makeDefaultActivity();
    runtime->counters = makeDefaultCounters();
    runtime->bgm_owner = nullptr;
    runtime->last_error = nullptr;
    return runtime;
}

oatpp::Object<creatures::CreatureRuntimeDto> getOrCreateRuntime(const std::string &creatureId) {
    std::lock_guard<std::mutex> lock(runtimeMutex);
    auto it = runtimeState.find(creatureId);
    if (it != runtimeState.end()) {
        return it->second;
    }
    auto runtime = makeDefaultRuntime();
    runtimeState.emplace(creatureId, runtime);
    return runtime;
}

oatpp::String resolveCreatureName(const std::string &creatureId) {
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        auto it = creatureNameCache.find(creatureId);
        if (it != creatureNameCache.end()) {
            return it->second.c_str();
        }
    }

    if (!creatures::db) {
        warn("CreatureService: database unavailable while resolving creature name for {}, using ID fallback",
             creatureId);
        return creatureId.c_str();
    }

    auto creatureResult = creatures::db->getCreature(creatureId, nullptr);
    if (!creatureResult.isSuccess()) {
        warn("CreatureService: failed to resolve creature name for {}: {}", creatureId,
             creatureResult.getError()->getMessage());
        return creatureId.c_str();
    }

    auto creature = creatureResult.getValue().value();
    {
        std::lock_guard<std::mutex> lock(runtimeMutex);
        creatureNameCache.emplace(creatureId, creature.name);
    }
    return creature.name.c_str();
}

void broadcastIdleStateChanged(const std::string &creatureId, bool enabled) {
    if (!creatures::websocketOutgoingMessages) {
        warn("CreatureService: websocket queue unavailable, skipping idle state broadcast for {}", creatureId);
        return;
    }

    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    auto msg = creatures::ws::IdleStateChangedMessage::createShared();
    msg->command = toString(creatures::ws::MessageType::IdleStateChanged).c_str();

    auto payload = creatures::ws::IdleStateChangedDto::createShared();
    payload->creature_id = creatureId.c_str();
    payload->idle_enabled = enabled;
    payload->timestamp = getCurrentTimeISO8601();
    msg->payload = payload;

    creatures::websocketOutgoingMessages->enqueue(jsonMapper->writeToString(msg));
}

void broadcastCreatureActivity(const std::string &creatureId, const oatpp::Object<creatures::CreatureRuntimeDto> &rt) {
    if (!rt || !rt->activity) {
        return;
    }
    if (!creatures::websocketOutgoingMessages) {
        warn("CreatureService: websocket queue unavailable, skipping activity broadcast for {}", creatureId);
        return;
    }
    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    auto msg = creatures::ws::CreatureActivityMessage::createShared();
    msg->command = toString(creatures::ws::MessageType::CreatureActivity).c_str();

    auto payload = creatures::ws::CreatureActivityDto::createShared();
    payload->creature_id = creatureId.c_str();
    payload->creature_name = resolveCreatureName(creatureId);
    payload->state = rt->activity->state;
    payload->animation_id = rt->activity->animation_id;
    payload->session_id = rt->activity->session_id;
    payload->reason = rt->activity->reason;
    payload->timestamp = getCurrentTimeISO8601();

    msg->payload = payload;
    creatures::websocketOutgoingMessages->enqueue(jsonMapper->writeToString(msg));
}

} // namespace

bool CreatureService::isCreatureStreaming(const creatureId_t &creatureId) {
    auto runtime = getOrCreateRuntime(creatureId);
    if (!runtime || !runtime->activity) {
        return false;
    }
    auto reasonStr = runtime->activity->reason;
    auto stateStr = runtime->activity->state;
    if (!reasonStr || !stateStr) {
        return false;
    }
    return reasonStr == creatures::runtime::toString(creatures::runtime::ActivityReason::Streaming) &&
           stateStr == creatures::runtime::toString(creatures::runtime::ActivityState::Running);
}

oatpp::Object<ListDto<oatpp::Object<creatures::CreatureDto>>>
CreatureService::getAllCreatures(std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    if (!parentSpan) {
        warn("no parent span provided for CreatureService.getAllCreatures, creating a root span");
    }

    // 🐰 Create a trace span for this request
    auto span = creatures::observability->createOperationSpan("CreatureService.getAllCreatures", std::move(parentSpan));

    appLogger->debug("CreatureService::getAllCreatures()");

    if (span) {
        trace("adding attributes to the span for CreatureService.getAllCreatures");
        span->setAttribute("endpoint", "getAllCreatures");
        span->setAttribute("ws_service", "CreatureService");
    }

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    auto result = db->getAllCreatures(creatures::SortBy::name, true, span); // Pass the span to the database call
    if (!result.isSuccess()) {

        // If we get an error, let's set it up right
        auto errorCode = result.getError().value().getCode();
        switch (errorCode) {
        case ServerError::NotFound:
            status = Status::CODE_404;
            break;
        case ServerError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
        errorMessage = result.getError()->getMessage();
        appLogger->warn(std::string(result.getError()->getMessage()));

        // Update the span with the error
        if (span) {
            span->setError(std::string(errorMessage));
            span->setAttribute("error.type", [errorCode]() {
                switch (errorCode) {
                case ServerError::NotFound:
                    return "NotFound";
                case ServerError::InvalidData:
                    return "InvalidData";
                case ServerError::DatabaseError:
                    return "DatabaseError";
                default:
                    return "InternalError";
                }
            }());
            span->setAttribute("error.code", static_cast<int64_t>(errorCode));
        }

        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    auto items = oatpp::Vector<oatpp::Object<creatures::CreatureDto>>::createShared();

    auto creatures = result.getValue().value();
    for (const auto &creature : creatures) {
        appLogger->debug("Adding creature: {}", creature.id);
        auto dto = creatures::convertToDto(creature);
        dto->runtime = getOrCreateRuntime(creature.id);
        items->emplace_back(dto);
    }

    auto page = ListDto<oatpp::Object<creatures::CreatureDto>>::createShared();
    page->count = items->size();
    page->items = items;

    // Record success metrics in the span
    if (span) {
        span->setAttribute("creatures.count", static_cast<int64_t>(page->count));
        span->setSuccess();
    }

    return page;
}

oatpp::Object<creatures::CreatureDto> CreatureService::getCreature(const oatpp::String &inCreatureId,
                                                                   std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    // Convert the oatpp string to a std::string
    creatureId_t creatureId = std::string(inCreatureId);

    appLogger->debug("CreatureService::getCreature({})", creatureId);

    auto span = creatures::observability->createOperationSpan("CreatureService.getCreature", parentSpan);

    if (span) {
        span->setAttribute("service", "CreatureService");
        span->setAttribute("operation", "getCreature");
        span->setAttribute("creature.id", std::string(creatureId));
    }

    debug("get creature by ID via REST API: {}", std::string(creatureId));

    if (span) {
        span->setAttribute("creature.id", std::string(creatureId));
    }

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    auto result = db->getCreature(creatureId, span); // Pass the span to the database call
    if (!result.isSuccess()) {

        // If we get an error, let's set it up right
        auto errorCode = result.getError().value().getCode();
        switch (errorCode) {
        case ServerError::NotFound:
            status = Status::CODE_404;
            break;
        case ServerError::InvalidData:
            status = Status::CODE_400;
            break;
        default:
            status = Status::CODE_500;
            break;
        }
        errorMessage = result.getError()->getMessage();
        appLogger->warn(std::string(result.getError()->getMessage()));

        if (span) {
            span->setError(std::string(errorMessage));
            span->setAttribute("error.type", [errorCode]() {
                switch (errorCode) {
                case ServerError::NotFound:
                    return "NotFound";
                case ServerError::InvalidData:
                    return "InvalidData";
                case ServerError::DatabaseError:
                    return "DatabaseError";
                default:
                    return "InternalError";
                }
            }());
            span->setAttribute("error.code", static_cast<int64_t>(errorCode));
        }

        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    auto creature = result.getValue().value();

    if (span) {
        span->setAttribute("creature.name", creature.name);
        span->setSuccess();
    }

    auto dto = creatures::convertToDto(creature);
    dto->runtime = getOrCreateRuntime(creature.id);
    return dto;
}

oatpp::Object<creatures::CreatureDto> CreatureService::upsertCreature(const std::string &jsonCreature,
                                                                      std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    auto serviceSpan = creatures::observability->createOperationSpan("CreatureService.upsertCreature", parentSpan);

    appLogger->info("attempting to upsert a creature");

    if (serviceSpan) {
        serviceSpan->setAttribute("service", "CreatureService");
        serviceSpan->setAttribute("operation", "upsertCreature");
        serviceSpan->setAttribute("json.size", static_cast<int64_t>(jsonCreature.length()));
    }
    appLogger->trace("JSON: {}", jsonCreature);

    bool error = false;
    oatpp::String errorMessage;
    Status status = Status::CODE_200;

    // ✨ Create a span for the validation step in the service
    auto validationSpan =
        creatures::observability->createChildOperationSpan("CreatureService.validateJson", serviceSpan);

    // Validate the JSON
    try {

        /*
         * This is a bit weird. Yes we're parsing the JSON twice. Once here, and once in upsertCreature().
         * This is because we need to validate the JSON before we pass it off to the database. If we don't,
         * we'll get a cryptic error from the database that doesn't tell us what's wrong with the JSON.
         *
         * We have to do this twice because the database stores whatever the client gives us. This means
         * that we need to pass in the raw JSON, but we also need to validate it here.
         */
        auto jsonResult = JsonParser::parseJsonString(jsonCreature, "creature upsert validation");
        if (!jsonResult.isSuccess()) {
            auto parseError = jsonResult.getError().value();
            errorMessage = parseError.getMessage();
            appLogger->warn(std::string(errorMessage));
            status = Status::CODE_400;
            error = true;
        }
        OATPP_ASSERT_HTTP(!error, status, errorMessage)
        auto jsonObject = jsonResult.getValue().value();
        auto result = db->validateCreatureJson(jsonObject);
        if (validationSpan)
            validationSpan->setAttribute("validator", "validateCreatureJson");

        if (!result.isSuccess()) {
            errorMessage = result.getError()->getMessage();
            appLogger->warn(std::string(result.getError()->getMessage()));
            status = Status::CODE_400;
            if (validationSpan)
                validationSpan->setError(std::string(errorMessage));
            if (serviceSpan)
                serviceSpan->setError(std::string(errorMessage));
            error = true;
        }
    } catch (const nlohmann::json::parse_error &e) {
        errorMessage = e.what();
        appLogger->warn(std::string(e.what()));
        status = Status::CODE_400;
        if (validationSpan)
            validationSpan->recordException(e);
        if (serviceSpan)
            serviceSpan->recordException(e);
        error = true;
    }
    if (validationSpan && !error)
        validationSpan->setSuccess();
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    appLogger->debug("passing the upsert request off to the database");
    auto result = db->upsertCreature(jsonCreature, serviceSpan);

    // If there's an error, let the client know
    if (!result.isSuccess()) {

        errorMessage = result.getError()->getMessage();
        appLogger->warn(std::string(result.getError()->getMessage()));
        status = Status::CODE_500;
        if (serviceSpan)
            serviceSpan->setError(std::string(errorMessage));
        error = true;
    }
    OATPP_ASSERT_HTTP(!error, status, errorMessage)

    // This should never happen and is a bad bug if it does 😱
    if (!result.getValue().has_value()) {
        errorMessage = "DB didn't return a value after upserting the creature. This is a bug. Please report it.";
        appLogger->error(std::string(errorMessage));
        if (serviceSpan)
            serviceSpan->setError(std::string(errorMessage));
        OATPP_ASSERT_HTTP(true, Status::CODE_500, errorMessage);
    }

    // Yay! All good! Send it along
    auto creature = result.getValue().value();
    info("Updated {} in the database", creature.name);
    if (serviceSpan) {
        serviceSpan->setAttribute("creature.id", creature.id);
        serviceSpan->setAttribute("creature.name", creature.name);
        serviceSpan->setSuccess();
    }

    auto dto = convertToDto(creature);
    dto->runtime = getOrCreateRuntime(creature.id);
    return dto;
}

oatpp::Object<creatures::CreatureDto> CreatureService::registerCreature(const std::string &jsonCreature,
                                                                        universe_t universe,
                                                                        std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    if (!parentSpan) {
        warn("no parent span provided for CreatureService.registerCreature, creating a root span");
    }

    auto serviceSpan = creatures::observability->createOperationSpan("CreatureService.registerCreature", parentSpan);

    appLogger->info("Controller registering creature with universe {}", universe);

    if (serviceSpan) {
        serviceSpan->setAttribute("service", "CreatureService");
        serviceSpan->setAttribute("operation", "registerCreature");
        serviceSpan->setAttribute("universe", static_cast<int64_t>(universe));
        serviceSpan->setAttribute("json.size", static_cast<int64_t>(jsonCreature.length()));
    }

    // First, upsert the creature to the database (this handles validation)
    auto creatureDto = upsertCreature(jsonCreature, parentSpan);

    // Defensive check: ensure we got a valid creature DTO back
    if (!creatureDto) {
        std::string errorMessage = "Invalid creature configuration provided";
        warn(errorMessage);
        if (serviceSpan) {
            serviceSpan->setError(errorMessage);
        }
        OATPP_ASSERT_HTTP(false, Status::CODE_400, errorMessage.c_str());
    }

    // Now store the creature-to-universe mapping in runtime memory
    std::string creatureId = std::string(creatureDto->id);
    creatures::creatureUniverseMap->put(creatureId, universe);

    appLogger->info("Registered creature '{}' (id: {}) on universe {}", std::string(creatureDto->name), creatureId,
                    universe);

    if (serviceSpan) {
        serviceSpan->setAttribute("creature.id", creatureId);
        serviceSpan->setAttribute("creature.name", std::string(creatureDto->name));
        serviceSpan->setSuccess();
    }

    return creatureDto;
}

oatpp::Object<creatures::CreatureDto> CreatureService::setIdleEnabled(const oatpp::String &inCreatureId, bool enabled,
                                                                      std::shared_ptr<RequestSpan> parentSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    creatureId_t creatureId = std::string(inCreatureId);
    appLogger->info("Setting idle {} for creature {}", enabled ? "enabled" : "disabled", creatureId);

    auto span = creatures::observability->createOperationSpan("CreatureService.setIdleEnabled", parentSpan);
    if (span) {
        span->setAttribute("creature.id", creatureId);
        span->setAttribute("idle.enabled", enabled);
    }

    // Ensure creature exists
    auto creatureResult = db->getCreature(creatureId, span);
    OATPP_ASSERT_HTTP(creatureResult.isSuccess(), Status::CODE_404, creatureResult.getError()->getMessage().c_str());

    // Update runtime state
    auto runtime = getOrCreateRuntime(creatureId);
    runtime->idle_enabled = enabled;
    if (!runtime->activity) {
        runtime->activity = makeDefaultActivity();
    }
    runtime->activity->animation_id = nullptr;
    runtime->activity->session_id = nullptr;
    runtime->activity->reason = creatures::runtime::toString(enabled ? creatures::runtime::ActivityReason::Idle
                                                                     : creatures::runtime::ActivityReason::Disabled);
    runtime->activity->state = creatures::runtime::toString(enabled ? creatures::runtime::ActivityState::Idle
                                                                    : creatures::runtime::ActivityState::Disabled);
    auto now = getCurrentTimeISO8601();
    runtime->activity->started_at = now;
    runtime->activity->updated_at = now;
    if (runtime->counters) {
        v_uint64 current = runtime->counters->idle_toggles_total ? *runtime->counters->idle_toggles_total : 0;
        runtime->counters->idle_toggles_total = static_cast<v_uint64>(current + 1);
        if (!enabled) {
            // Treat disabling idle as a stop event
            v_uint64 stopped = runtime->counters->idle_stopped_total ? *runtime->counters->idle_stopped_total : 0;
            runtime->counters->idle_stopped_total = static_cast<v_uint64>(stopped + 1);
        }
    }

    // Build DTO response
    auto dto = creatures::convertToDto(creatureResult.getValue().value());
    dto->runtime = runtime;

    broadcastIdleStateChanged(creatureId, enabled);
    broadcastCreatureActivity(creatureId, runtime);

    if (span) {
        span->setSuccess();
    }

    return dto;
}

// Helper to decide idle vs disabled state when attempting to mark idle
static creatures::runtime::ActivityState resolveIdleState(const oatpp::Object<creatures::CreatureRuntimeDto> &runtime,
                                                          creatures::runtime::ActivityState requested) {
    bool idleEnabled = !runtime->idle_enabled || *runtime->idle_enabled;
    if (requested == creatures::runtime::ActivityState::Idle && !idleEnabled) {
        return creatures::runtime::ActivityState::Disabled;
    }
    return requested;
}

std::string CreatureService::setActivityState(const std::vector<creatureId_t> &creatureIds,
                                              const std::string &animationId, runtime::ActivityReason reason,
                                              runtime::ActivityState state, const std::string &sessionId,
                                              std::shared_ptr<OperationSpan> /*parentSpan*/) {

    // Generate or reuse session ID
    std::string sid = sessionId.empty() ? creatures::util::generateUUID() : sessionId;
    auto now = getCurrentTimeISO8601();

    for (const auto &creatureId : creatureIds) {
        if (creatureId.empty()) {
            continue;
        }
        auto runtime = getOrCreateRuntime(creatureId);
        if (!runtime->activity) {
            runtime->activity = makeDefaultActivity();
        }

        auto resolvedState = resolveIdleState(runtime, state);
        auto resolvedReason = reason;
        if (resolvedState == creatures::runtime::ActivityState::Disabled &&
            state == creatures::runtime::ActivityState::Idle) {
            resolvedReason = creatures::runtime::ActivityReason::Disabled;
        }

        runtime->activity->state = creatures::runtime::toString(resolvedState);
        if (resolvedState == creatures::runtime::ActivityState::Running) {
            runtime->activity->animation_id = animationId.c_str();
            runtime->activity->session_id = sid.c_str();
        } else {
            runtime->activity->animation_id = nullptr;
            runtime->activity->session_id = nullptr;
        }
        runtime->activity->reason = creatures::runtime::toString(resolvedReason);
        runtime->activity->started_at = now;
        runtime->activity->updated_at = now;

        if (runtime->counters) {
            if (resolvedState == creatures::runtime::ActivityState::Running) {
                v_uint64 current =
                    runtime->counters->sessions_started_total ? *runtime->counters->sessions_started_total : 0;
                runtime->counters->sessions_started_total = static_cast<v_uint64>(current + 1);
            }
            if (resolvedReason == creatures::runtime::ActivityReason::Cancelled) {
                v_uint64 cancelled =
                    runtime->counters->sessions_cancelled_total ? *runtime->counters->sessions_cancelled_total : 0;
                runtime->counters->sessions_cancelled_total = static_cast<v_uint64>(cancelled + 1);
            }
        }

        broadcastCreatureActivity(creatureId, runtime);
    }

    return sid;
}

std::string CreatureService::setActivityRunning(const std::vector<creatureId_t> &creatureIds,
                                                const std::string &animationId, runtime::ActivityReason reason,
                                                const std::string &sessionId,
                                                std::shared_ptr<OperationSpan> parentSpan) {
    return setActivityState(creatureIds, animationId, reason, creatures::runtime::ActivityState::Running, sessionId,
                            parentSpan);
}

} // namespace creatures::ws
