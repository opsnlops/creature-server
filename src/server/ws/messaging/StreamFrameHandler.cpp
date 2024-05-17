
#include <spdlog/spdlog.h>



#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>


#include "model/StreamFrame.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
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
}

namespace creatures ::ws {

    void StreamFrameHandler::processMessage(const oatpp::String &message) {

        try {

#ifdef STREAM_FRAME_DEBUG
            appLogger->debug("Decoding into a streamed frame: {}", std::string(message));
#endif

            auto dto = apiObjectMapper->readFromString<oatpp::Object<creatures::ws::StreamFrameCommandDTO>>(message);
            if (dto) {
                StreamFrame frame = convertFromDto(dto->payload.getPtr());
                stream(frame);
            } else {
                appLogger->warn("unable to cast an incoming message to 'Notice'");
            }

        } catch (const std::bad_cast &e) {
            appLogger->warn("Error (std::bad_cast) while processing '{}' into a StreamFrame message: {} ",
                            std::string(message), e.what());
        } catch (const std::exception &e) {
            appLogger->warn("Error (std::exception) while processing '{}' into a StreamFrame message: {}",
                            std::string(message), e.what());
        } catch (...) {
            appLogger->warn("An unknown error happened while processing '{}' into a StreamFrame message",
                            std::string(message));
        }

    }


    void StreamFrameHandler::stream(creatures::StreamFrame frame) {

        appLogger->trace("Entered StreamFrameHandler::stream()");

        // Make sure this creature is in the cache
        std::shared_ptr<Creature> creature;
        try {
            creature = creatureCache->get(frame.creature_id);
        } catch (const std::out_of_range &e) {
            appLogger->debug(" 🛜  creature {} was not found in the cache. Going to the DB...", frame.creature_id);
            creature = std::make_shared<Creature>(db->getCreature(frame.creature_id));
            appLogger->debug("creature is now: name: {}, channel_offset: {}", creature->name, creature->channel_offset);
        }

        // Make sure it's valid before we go on
        if(!creature) {
            appLogger->warn("Dropping stream frame to {} because it can't be found", frame.creature_id);
            return;
        }

        //appLogger->debug("Creature: {}, Offset: {}", creature->name, creature->channel_offset);

        // Parse this out
        auto frameData = decodeBase64(frame.data);

#ifdef STREAM_FRAME_DEBUG
        appLogger->debug("Requested frame data: {}", vectorToHexString(frameData));
#endif

        auto event = std::make_shared<DMXEvent>(eventLoop->getNextFrameNumber());
        event->universe = frame.universe;
        event->channelOffset = creature->channel_offset;
        event->data.reserve(frameData.size());

        //appLogger->debug("universe: {}, channelOffset: {}", event->universe, event->channelOffset);

        for (uint8_t byte: frameData) {

#ifdef STREAM_FRAME_DEBUG
            trace("byte {}: 0x{:02x}", i++, byte);
#endif
            event->data.push_back(byte);
        }



        eventLoop->scheduleEvent(event);
        metrics->incrementFramesStreamed();

        // Keep some metrics internally
        framesStreamed += 1;
        if(framesStreamed % 500 == 0) {
            debug("streamed {} frames", framesStreamed);
        }
    }

}