
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

#include "util/helpers.h"

namespace creatures {

    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<Database> db;

    /**
     * Send frames from a client to a Creature
     */
//    grpc::Status CreatureServerImpl::StreamFrames(ServerContext *context,
//                                                  ServerReader<StreamFrameData>* reader,
//                                                  StreamFrameDataResponse* response) {
//
//        info("a request to receive frames has come in");
//        StreamFrameData frame;
//        int32_t frame_count = 0;
//
//        extern std::shared_ptr<EventLoop> eventLoop;
//
//        // Grab the first one now, so we can log it
//        reader->Read(&frame);
//
//
//        // Look up this creature in the database
//        CreatureId creatureId = stringToCreatureId(frame.creature_id());
//
//        // Load the creature
//        server::Creature creature;
//        try {
//            db->gRPCgetCreature(&creatureId, &creature);
//        }
//        catch( const NotFoundException& e) {
//            error("Creature {} not found", frame.creature_id());
//            return Status{grpc::StatusCode::NOT_FOUND, e.what()};
//        }
//        catch( const DataFormatException& e) {
//            error("Data format exception while loading creature {}: {}", frame.creature_id(), e.what());
//            return Status{grpc::StatusCode::INTERNAL, e.what()};
//        }
//        catch( ... ) {
//            error("Unknown error while loading creature {}", frame.creature_id());
//            return Status{grpc::StatusCode::INTERNAL, "Unknown error"};
//        }
//
//        // Woo, we found it!
//        info("sending frames to {}", creature.name());
//
//
//        // Process the incoming stream of frames
//        do {
//#if DEBUG_STREAM_FRAMES
//            trace("received frame {} for {}", frame_count, creature.name());
//#endif
//            const std::string &frame_data = frame.data();
//
//            // Create a new event and schedule it for the next frame
//            auto event = std::make_shared<DMXEvent>(eventLoop->getNextFrameNumber());
//            event->universe = frame.universe();
//            event->channelOffset = creature.channel_offset();
//            event->data.reserve(frame_data.size());
//
//#if DEBUG_STREAM_FRAMES
//            trace("creature {} a channel offset of {}",
//                  creature.name(), creature.channel_offset());
//#endif
//
//            uint8_t i = 0;
//            for (uint8_t byte: frame_data) {
//#if DEBUG_STREAM_FRAMES
//                trace("byte {}: 0x{:02x}", i++, byte);
//#endif
//                event->data.push_back(byte);
//            }
//#if DEBUG_STREAM_FRAMES
//            trace("DMX event created");
//#endif
//            eventLoop->scheduleEvent(event);
//
//            // Log a message every 100 frames
//            if (++frame_count % 100 == 0)
//                debug("transmitted {} frames to {}", frame_count, creature.name());
//
//            metrics->incrementFramesStreamed();
//
//        } while (reader->Read(&frame));
//
//        info("end of frames from client. {} total", frame_count);
//
//        // Set the response
//        response->set_frames_processed(frame_count);
//        response->set_message(fmt::format("{} frames processed successfully", frame_count));
//
//        return Status::OK;
//    }

}

