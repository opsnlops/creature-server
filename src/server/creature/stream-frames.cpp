
#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/dmx/dmx.h"


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

        // Grab the first one now, so we can set up the DMX client
        reader->Read(&frame);
        auto sender = std::make_unique<DMX>();
        trace("sender made");

        sender->init(frame.sacn_ip(), frame.universe(), frame.number_of_motors());
        info("sending frames to {}", frame.creature_name());

        // Process the incoming stream of frames
        do {

            trace("received frame {} for {}", frame_count, frame.creature_name());
            const std::string &frame_data = frame.frame();
            uint8_t buffer[frame.number_of_motors()];

            int i = 0;
            for (uint8_t byte: frame_data) {
                trace("byte {}: 0x{:02x}", i, byte);
                buffer[i++] = byte;
            }
            trace("frame transmitted");

            sender->send(buffer, frame.number_of_motors());

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

