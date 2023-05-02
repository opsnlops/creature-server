
#include "spdlog/spdlog.h"

#include "server/config.h"
#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

namespace creatures {

    /**
     * Send frames from a client to a Creature
     */
    grpc::Status CreatureServerImpl::StreamFrames(ServerContext *context,
                                                  ServerReader<Frame> *reader,
                                                  FrameResponse *response) {

        info("a request to receive frames has come in");
        Frame frame;
        int32_t frame_count = 0;

        extern std::shared_ptr<EventLoop> eventLoop;

        // Grab the first one now, so we can set up the DMX client
        reader->Read(&frame);
        //auto sender = std::make_unique<DMX>();
        trace("sender made");

        //sender->init(frame.sacn_ip(), frame.universe(), frame.number_of_motors());
        info("sending frames to {}", frame.creature_name());

        // Process the incoming stream of frames
        do {

            trace("received frame {} for {}", frame_count, frame.creature_name());
            const std::string &frame_data = frame.frame();

            // Create a new event and schedule it for the next frame
            auto event = std::make_shared<DMXEvent>(eventLoop->getNextFrameNumber());
            event->clientIP = frame.sacn_ip();
            event->numMotors = frame.number_of_motors();
            event->dmxUniverse = frame.universe();
            event->dmxOffset = frame.dmx_offset();
            event->data.reserve(frame.number_of_motors());

            for (uint8_t byte: frame_data) {
#if DEBUG_STREAM_FRAMES
                trace("byte {}: 0x{:02x}", i, byte);
#endif
                event->data.push_back(byte);
            }
            trace("DMX event created");

            eventLoop->scheduleEvent(event);

            // Log a message every 100 frames
            if (++frame_count % 100 == 0)
                debug("transmitted {} frames to {}", frame_count, frame.creature_name());

        } while (reader->Read(&frame));

        info("end of frames from client. {} total", frame_count);

        // Set the response
        response->set_frames_processed(frame_count);
        response->set_message(fmt::format("{} frames processed successfully", frame_count));

        return Status::OK;
    }

}

