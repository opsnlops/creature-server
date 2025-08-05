
#include <spdlog/spdlog.h>

#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/StreamFrame.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
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
} // namespace creatures

namespace creatures ::ws {

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

    // Make sure this creature is in the cache
    std::shared_ptr<Creature> creature;
    try {
        creature = creatureCache->get(frame.creature_id);
        span->setAttribute("creature_cache.hit", true);
    } catch (const std::out_of_range &e) {
        span->setAttribute("creature_cache.hit", false);
        appLogger->debug(" 🛜  creature {} was not found in the cache. Going to the DB...", frame.creature_id);

        // Convert SamplingSpan to OperationSpan for database call
        std::shared_ptr<OperationSpan> operationSpan = std::static_pointer_cast<OperationSpan>(span);
        auto result = db->getCreature(frame.creature_id, operationSpan);
        if (!result.isSuccess()) {
            auto errorMessage = fmt::format("Dropping stream frame to {} because it can't be found: {}",
                                            frame.creature_id, result.getError().value().getMessage());
            appLogger->warn(errorMessage);
            span->setError(errorMessage);
            span->setAttribute("error.type", "NotFound");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::NotFound));
            return;
        }
        creature = std::make_shared<Creature>(result.getValue().value());
        appLogger->debug("creature is now: name: {}, channel_offset: {}", creature->name, creature->channel_offset);
        span->setSuccess();
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