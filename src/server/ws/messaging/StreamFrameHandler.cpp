
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
std::unordered_set<creatureId_t> streamingCreatures;
std::unordered_map<creatureId_t, framenum_t> streamingDeadlines;

bool markStreamingIfNew(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    auto [it, inserted] = streamingCreatures.insert(creatureId);
    return inserted;
}

void updateStreamingDeadline(const creatureId_t &creatureId, framenum_t deadline) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    streamingDeadlines[creatureId] = deadline;
}

std::optional<framenum_t> getStreamingDeadline(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    auto it = streamingDeadlines.find(creatureId);
    if (it == streamingDeadlines.end()) {
        return std::nullopt;
    }
    return it->second;
}

void clearStreamingState(const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(streamingMutex);
    streamingCreatures.erase(creatureId);
    streamingDeadlines.erase(creatureId);
}

class StreamingTimeoutEvent : public EventBase<StreamingTimeoutEvent> {
  public:
    StreamingTimeoutEvent(framenum_t frame, creatureId_t creatureId)
        : EventBase<StreamingTimeoutEvent>(frame), creatureId_(std::move(creatureId)) {}

    Result<framenum_t> executeImpl() {
        auto deadline = getStreamingDeadline(creatureId_);
        if (!deadline.has_value()) {
            return Result<framenum_t>{this->frameNumber};
        }
        // If another frame extended the deadline, skip
        if (deadline.value() != this->frameNumber) {
            return Result<framenum_t>{this->frameNumber};
        }

        // Mark streaming stopped
        creatures::ws::CreatureService::setActivityState(
            {creatureId_}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
            creatures::runtime::ActivityState::Stopped, "" /*sessionId*/, nullptr);
        info("Streaming timeout reached for creature {} at frame {}", creatureId_, this->frameNumber);
        clearStreamingState(creatureId_);
        creatures::ws::CreatureService::startIdleIfNeeded(creatureId_, nullptr);
        return Result<framenum_t>{this->frameNumber};
    }

  private:
    creatureId_t creatureId_;
};
} // namespace

void StreamFrameHandler::processMessage(const oatpp::String &message) {

    // Create a sampling span for this high-frequency operation (uses default 0.01% sampling rate)
    auto messageSpan = creatures::observability->createSamplingSpan("StreamFrameHandler.processMessage");

    try {

#ifdef STREAM_FRAME_DEBUG
        appLogger->debug("Decoding into a streamed frame: {}", std::string(message));
#endif

        auto dto = apiObjectMapper->readFromString<oatpp::Object<creatures::ws::StreamFrameCommandDTO>>(message);
        if (dto) {
            messageSpan->setAttribute("phase", "processing");
            StreamFrame frame = convertFromDto(dto->payload.getPtr() /*, messageSpan */); // This is super fast, no span
            messageSpan->setAttribute("phase", "streaming");
            stream(frame, messageSpan);
            messageSpan->setSuccess();
        } else {
            appLogger->warn("unable to cast an incoming message to 'Notice'");
        }

    } catch (const std::bad_cast &e) {
        auto errorMessage = fmt::format("Error (std::bad_cast) while processing '{}' into a StreamFrame message: {} ",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::bad_cast");
        messageSpan->setAttribute("error.message", errorMessage);
        messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    } catch (const std::exception &e) {
        auto errorMessage = fmt::format("Error (std::exception) while processing '{}' into a StreamFrame message: {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::exception");
        messageSpan->setAttribute("error.message", errorMessage);
        messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    } catch (...) {
        auto errorMessage = fmt::format("An unknown error happened while processing '{}' into a StreamFrame message",
                                        std::string(message));
        appLogger->warn(errorMessage);
        messageSpan->setError(errorMessage);
        messageSpan->setAttribute("error.type", "std::bad_cast");
        messageSpan->setAttribute("error.message", errorMessage);
        messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
    }
}

void StreamFrameHandler::stream(creatures::StreamFrame frame, std::shared_ptr<SamplingSpan> parentSpan) {

    // Use the parent sampling span instead of creating a child span for this high-frequency operation
    auto span = parentSpan;

    appLogger->trace("Entered StreamFrameHandler::stream()");

    // Cancel any active playback on this universe for the targeted creature (live streaming takes priority)
    creatures::sessionManager->cancelSessionsForCreatures(frame.universe, {frame.creature_id});
    span->setAttribute("cancelled_session_for_streaming", true);

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
        span->setAttribute("creature_cache.hit", true);
    } catch (const std::out_of_range &e) {
        span->setAttribute("creature_cache.hit", false);
        appLogger->debug(" 🛜  creature {} was not found in the cache. Going to the DB...", frame.creature_id);

        // Create a child span specifically for the database fallback operation
        auto dbFallbackSpan = creatures::observability->createChildOperationSpan(
            "StreamFrameHandler.creatureCacheMiss", std::static_pointer_cast<OperationSpan>(span));
        if (dbFallbackSpan) {
            dbFallbackSpan->setAttribute("creature.id", frame.creature_id);
            dbFallbackSpan->setAttribute("cache_miss", true);
        }

        auto result = db->getCreature(frame.creature_id, dbFallbackSpan);
        if (!result.isSuccess()) {
            auto errorMessage = fmt::format("Dropping stream frame to {} because it can't be found: {}",
                                            frame.creature_id, result.getError().value().getMessage());
            appLogger->warn(errorMessage);
            if (dbFallbackSpan) {
                dbFallbackSpan->setError(errorMessage);
            }
            span->setError(errorMessage);
            span->setAttribute("error.type", "NotFound");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            return;
        }
        creature = std::make_shared<Creature>(result.getValue().value());
        appLogger->debug("creature is now: name: {}, channel_offset: {}", creature->name, creature->channel_offset);

        if (dbFallbackSpan) {
            dbFallbackSpan->setAttribute("creature.name", creature->name);
            dbFallbackSpan->setSuccess();
        }
        span->setAttribute("creature.name", creature->name);
    }

    // Make sure it's valid before we go on
    if (!creature) {
        auto errorMessage = fmt::format("Creature {} was not found in the cache or the database", frame.creature_id);
        appLogger->warn(errorMessage);
        span->setError(errorMessage);
        span->setAttribute("error.type", "NotFound");
        span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
        return;
    }

    // Mark runtime activity as streaming (only once per creature)
    if (markStreamingIfNew(frame.creature_id)) {
        creatures::ws::CreatureService::setActivityState(
            {frame.creature_id}, "" /*animationId*/, creatures::runtime::ActivityReason::Streaming,
            creatures::runtime::ActivityState::Running, "" /*sessionId*/, nullptr);
    }

    // Schedule a timeout to mark streaming stopped if frames stop arriving
    framenum_t streamingTimeoutFrames = DEFAULT_STREAMING_TIMEOUT_FRAMES;
    if (creatures::config) {
        streamingTimeoutFrames = creatures::config->getStreamingTimeoutFrames();
    }

    auto deadline = eventLoop->getNextFrameNumber() + streamingTimeoutFrames;
    updateStreamingDeadline(frame.creature_id, deadline);
    auto timeoutEvent = std::make_shared<StreamingTimeoutEvent>(deadline, frame.creature_id);
    eventLoop->scheduleEvent(timeoutEvent);

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

    span->setSuccess();
}

} // namespace creatures::ws
