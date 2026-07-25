
#include <spdlog/spdlog.h>

#include <mutex>
#include <optional>
#include <unordered_set>

#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/StreamFrame.h"
#include "server/animation/SessionManager.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

#include "StreamFrameCommandDTO.h"
#include "StreamFrameHandler.h"

/**
 * This handler does a lot of heavy lifting. Streaming from the console is one of the most
 * important things this server does. It's also called in rapid fire, almost exactly every 20ms
 * while we're being streamed to. (And we could have several Creatures being streamed to at
 * once!)
 *
 * Since this one is so important, it's pretty big and touches a lot of things. Tread lightly
 * if working in here! 💜
 */

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<SessionManager> sessionManager;
} // namespace creatures

namespace creatures ::ws {

namespace {
std::mutex streamingMutex;
std::unordered_map<creatureId_t, framenum_t> streamingDeadlines;
// Creatures with a StreamingTimeoutEvent already in flight. Frames arrive at 50Hz, so
// scheduling an event per frame would leave dozens queued per creature — instead exactly
// one event chases the (constantly extended) deadline per creature (issue #73).
// (Which creatures are *streaming* lives in CreatureService's registry, where the
// schedulers can see it — issue #74. Only the timeout bookkeeping lives here.)
std::unordered_set<creatureId_t> timeoutEventPending;
// When each streaming session started, so the session-end span can report a duration
// (once per session — zero cost on the 50Hz frame path).
std::unordered_map<creatureId_t, std::chrono::steady_clock::time_point> streamingStartedAt;

void markStreamingStarted(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    streamingStartedAt[creatureId] = std::chrono::steady_clock::now();
}

void updateStreamingDeadline(const creatureId_t &creatureId, framenum_t deadline) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    streamingDeadlines[creatureId] = deadline;
}

/// Returns true if the caller should schedule a new timeout event (none is in flight).
bool markTimeoutEventPending(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    auto [it, inserted] = timeoutEventPending.insert(creatureId);
    return inserted;
}

void clearTimeoutEventPending(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    timeoutEventPending.erase(creatureId);
}

/// The timeout event's atomic expire-or-chase decision. The deadline re-check and the
/// bookkeeping teardown happen under one lock, so a frame that extends the deadline
/// between the event firing and the teardown is never lost (it either moves us to
/// `chase`, or arrives after `expired` and does a full clean re-setup on its next frame).
struct ExpiryDecision {
    enum class Action { alreadyGone, chase, expired };
    Action action;
    framenum_t chaseDeadline{0};
    std::optional<std::chrono::steady_clock::time_point> startedAt{};
};

ExpiryDecision decideExpiry(const creatureId_t &creatureId, framenum_t eventFrame) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    auto it = streamingDeadlines.find(creatureId);
    if (it == streamingDeadlines.end()) {
        timeoutEventPending.erase(creatureId);
        return {ExpiryDecision::Action::alreadyGone};
    }
    if (it->second > eventFrame) {
        return {ExpiryDecision::Action::chase, it->second};
    }

    ExpiryDecision decision{ExpiryDecision::Action::expired};
    if (auto startIt = streamingStartedAt.find(creatureId); startIt != streamingStartedAt.end()) {
        decision.startedAt = startIt->second;
        streamingStartedAt.erase(startIt);
    }
    streamingDeadlines.erase(it);
    timeoutEventPending.erase(creatureId);
    return decision;
}

class StreamingTimeoutEvent : public EventBase<StreamingTimeoutEvent> {
  public:
    StreamingTimeoutEvent(framenum_t frame, creatureId_t creatureId, uint32_t chaseCount = 0)
        : EventBase<StreamingTimeoutEvent>(frame), creatureId_(std::move(creatureId)), chaseCount_(chaseCount) {}

    Result<framenum_t> executeImpl() {
        auto decision = decideExpiry(creatureId_, this->frameNumber);

        if (decision.action == ExpiryDecision::Action::alreadyGone) {
            return Result<framenum_t>{this->frameNumber};
        }

        // Frames kept arriving and pushed the deadline out — this event is the single
        // in-flight timeout for the creature, so chase the new deadline instead of dying.
        if (decision.action == ExpiryDecision::Action::chase) {
            if (eventLoop) {
                auto chase =
                    std::make_shared<StreamingTimeoutEvent>(decision.chaseDeadline, creatureId_, chaseCount_ + 1);
                eventLoop->scheduleEvent(chase);
            } else {
                // Shutdown: nothing will ever fire again, so don't leave the pending
                // gate set (a restart of streaming would otherwise never re-arm).
                clearTimeoutEventPending(creatureId_);
            }
            return Result<framenum_t>{this->frameNumber};
        }

        // Genuinely expired. Clear the registry FIRST: it's the load-bearing state. If
        // any of the broadcast/idle work below throws, the event loop's catch swallows
        // it — with the bookkeeping already cleared the next stream frame re-arms
        // cleanly instead of leaving the creature permanently "streaming" with no
        // timeout event ever scheduled again (security review, PR #75).
        creatures::ws::CreatureService::clearStreaming(creatureId_);

        // This fires once per streaming session, so a real (unsampled) span is cheap and
        // gives the session end a creature identity, a duration, and a parent for the
        // activity write and idle restart (no more NULL-parent orphan roots here).
        auto timeoutSpan = creatures::observability
                               ? creatures::observability->createOperationSpan("StreamFrameHandler.streamingTimeout")
                               : nullptr;
        if (timeoutSpan) {
            timeoutSpan->setAttribute("creature.id", creatureId_);
            timeoutSpan->setAttribute("creature.name",
                                      creatures::ws::CreatureService::resolveCreatureName(creatureId_));
            timeoutSpan->setAttribute("streaming.session.chase_count", static_cast<int64_t>(chaseCount_));
            if (decision.startedAt.has_value()) {
                auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - decision.startedAt.value())
                                      .count();
                timeoutSpan->setAttribute("streaming.session.duration_ms", static_cast<int64_t>(durationMs));
            }
        }

        creatures::ws::CreatureService::setActivityState(
            {creatureId_}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
            creatures::runtime::ActivityState::Stopped, "" /*sessionId*/, timeoutSpan);
        info("Streaming timeout reached for creature {} at frame {}", creatureId_, this->frameNumber);
        creatures::ws::CreatureService::startIdleIfNeeded(creatureId_, timeoutSpan);
        if (timeoutSpan) {
            timeoutSpan->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

  private:
    creatureId_t creatureId_;
    uint32_t chaseCount_;
};
} // namespace

void StreamFrameHandler::processMessage(const oatpp::String &message) {

    // Create a sampling span for this high-frequency operation. createSamplingSpan
    // returns nullptr when ObservabilityManager is uninitialized, so all subsequent
    // dereferences need to be guarded.
    auto messageSpan = creatures::observability
                           ? creatures::observability->createSamplingSpan("StreamFrameHandler.processMessage")
                           : nullptr;

    try {

#ifdef STREAM_FRAME_DEBUG
        appLogger->debug("Decoding into a streamed frame: {}", std::string(message));
#endif

        auto dto = apiObjectMapper->readFromString<oatpp::Object<creatures::ws::StreamFrameCommandDTO>>(message);
        if (dto) {
            if (messageSpan) {
                messageSpan->setAttribute("phase", "processing");
            }
            StreamFrame frame = convertFromDto(dto->payload.getPtr() /*, messageSpan */); // This is super fast, no span
            if (messageSpan) {
                messageSpan->setAttribute("phase", "streaming");
            }
            stream(frame, messageSpan);
            if (messageSpan) {
                messageSpan->setSuccess();
            }
        } else {
            appLogger->warn("unable to cast an incoming message to 'Notice'");
        }

    } catch (const std::bad_cast &e) {
        auto errorMessage = fmt::format("Error (std::bad_cast) while processing '{}' into a StreamFrame message: {} ",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        if (messageSpan) {
            messageSpan->recordException(e);
            messageSpan->setAttribute("error.type", "std::bad_cast");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
    } catch (const std::exception &e) {
        auto errorMessage = fmt::format("Error (std::exception) while processing '{}' into a StreamFrame message: {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        if (messageSpan) {
            messageSpan->recordException(e);
            messageSpan->setAttribute("error.type", "std::exception");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
    } catch (...) {
        auto errorMessage = fmt::format("An unknown error happened while processing '{}' into a StreamFrame message",
                                        std::string(message));
        appLogger->warn(errorMessage);
        if (messageSpan) {
            messageSpan->setError(errorMessage);
            messageSpan->setAttribute("error.type", "unknown");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
    }
}

void StreamFrameHandler::stream(creatures::StreamFrame frame, std::shared_ptr<SamplingSpan> parentSpan) {

    // Use the parent sampling span instead of creating a child span for this high-frequency operation.
    // `span` may be nullptr if observability is uninitialized — guard every dereference.
    auto span = parentSpan;

    appLogger->trace("Entered StreamFrameHandler::stream()");

    // Cancel any active playback on this universe for the targeted creature (live streaming takes priority)
    creatures::sessionManager->cancelSessionsForCreatures(frame.universe, {frame.creature_id});
    if (span) {
        span->setAttribute("streaming.preempted_session", true);
    }

    // Also stop any playlist state
    auto playlistState = creatures::sessionManager->getPlaylistState(frame.universe);
    if (playlistState == PlaylistState::Active || playlistState == PlaylistState::Interrupted) {
        appLogger->info("Stopping playlist on universe {} for live streaming", frame.universe);
        creatures::sessionManager->stopPlaylist(frame.universe);
    }

    // Make sure this creature is in the cache
    std::shared_ptr<Creature> creature;
    try {
        creature = creatureCache->get(frame.creature_id);
        if (span) {
            span->setAttribute("creature_cache.hit", true);
        }
    } catch (const std::out_of_range &e) {
        if (span) {
            span->setAttribute("creature_cache.hit", false);
        }
        appLogger->debug(" 🛜  creature {} was not found in the cache. Going to the DB...", frame.creature_id);

        // Create a child span specifically for the database fallback operation
        auto dbFallbackSpan =
            (creatures::observability && span)
                ? creatures::observability->createChildOperationSpan("StreamFrameHandler.creatureCacheMiss",
                                                                     std::static_pointer_cast<OperationSpan>(span))
                : nullptr;
        if (dbFallbackSpan) {
            dbFallbackSpan->setAttribute("creature.id", frame.creature_id);
            dbFallbackSpan->setAttribute("cache.result", std::string("miss"));
        }

        auto result = db->getCreature(frame.creature_id, dbFallbackSpan);
        if (!result.isSuccess()) {
            auto errorMessage = fmt::format("Dropping stream frame to {} because it can't be found: {}",
                                            frame.creature_id, result.getError().value().getMessage());
            appLogger->warn(errorMessage);
            if (dbFallbackSpan) {
                dbFallbackSpan->setError(errorMessage);
            }
            if (span) {
                span->setError(errorMessage);
                span->setAttribute("error.type", "NotFound");
                span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            }
            return;
        }
        creature = std::make_shared<Creature>(result.getValue().value());
        appLogger->debug("creature is now: name: {}, channel_offset: {}", creature->name, creature->channel_offset);

        if (dbFallbackSpan) {
            dbFallbackSpan->setAttribute("creature.name", creature->name);
            dbFallbackSpan->setSuccess();
        }
        if (span) {
            span->setAttribute("creature.name", creature->name);
        }
    }

    // Make sure it's valid before we go on
    if (!creature) {
        auto errorMessage = fmt::format("Creature {} was not found in the cache or the database", frame.creature_id);
        appLogger->warn(errorMessage);
        if (span) {
            span->setError(errorMessage);
            span->setAttribute("error.type", "NotFound");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
        }
        return;
    }

    // Mark runtime activity as streaming (only once per creature). Session start is a
    // once-per-session transition, so give it a real span — the sampled frame span is
    // null 99.95% of the time and would leave the activity write an orphan root.
    if (creatures::ws::CreatureService::markStreamingIfNew(frame.creature_id)) {
        markStreamingStarted(frame.creature_id);
        auto startSpan = creatures::observability
                             ? creatures::observability->createOperationSpan("StreamFrameHandler.streamingStarted")
                             : nullptr;
        if (startSpan) {
            startSpan->setAttribute("creature.id", frame.creature_id);
            startSpan->setAttribute("creature.name", creature->name);
            startSpan->setAttribute("streaming.universe", static_cast<int64_t>(frame.universe));
        }
        creatures::ws::CreatureService::setActivityState(
            {frame.creature_id}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
            creatures::runtime::ActivityState::Running, "" /*sessionId*/, startSpan);
        if (startSpan) {
            startSpan->setSuccess();
        }
    }

    // Schedule a timeout to mark streaming stopped if frames stop arriving
    framenum_t streamingTimeoutFrames = DEFAULT_STREAMING_TIMEOUT_FRAMES;
    if (creatures::config) {
        streamingTimeoutFrames = creatures::config->getStreamingTimeoutFrames();
    }

    auto deadline = eventLoop->getNextFrameNumber() + streamingTimeoutFrames;
    updateStreamingDeadline(frame.creature_id, deadline);
    // One timeout event per creature: if one is already in flight it will chase the
    // extended deadline itself when it fires (issue #73).
    if (markTimeoutEventPending(frame.creature_id)) {
        auto timeoutEvent = std::make_shared<StreamingTimeoutEvent>(deadline, frame.creature_id);
        eventLoop->scheduleEvent(timeoutEvent);
    }

    // appLogger->debug("Creature: {}, Offset: {}", creature->name, creature->channel_offset);

    // Parse this out
    auto frameData = decodeBase64(frame.data);

#ifdef STREAM_FRAME_DEBUG
    appLogger->debug("Requested frame data: {}", vectorToHexString(frameData));
#endif

    auto event = std::make_shared<DMXEvent>(eventLoop->getNextFrameNumber());
    event->universe = frame.universe;
    event->channelOffset = creature->channel_offset;
    event->data.reserve(frameData.size());

    // appLogger->debug("universe: {}, channelOffset: {}", event->universe, event->channelOffset);

    for (uint8_t byte : frameData) {

#ifdef STREAM_FRAME_DEBUG
        trace("byte {}: 0x{:02x}", i++, byte);
#endif
        event->data.push_back(byte);
    }

    eventLoop->scheduleEvent(event);

    // Update the global metrics
    metrics->incrementFramesStreamed();

    // Keep some metrics internally
    framesStreamed += 1;
    if (framesStreamed % 500 == 0) {
        debug("streamed {} frames", framesStreamed);
    }

    if (span) {
        span->setSuccess();
    }
}

} // namespace creatures::ws
