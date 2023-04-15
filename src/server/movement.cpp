

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/dmx.h"


using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;


Status creatures::CreatureServerImpl::StreamFrames(ServerContext* context,
                                                   ServerReader<Frame>* reader,
                                                   FrameResponse* response) {

    info("a request to receive frames has come in");
    Frame frame;
    int32_t frame_count = 0;

    // Grab the first one now, so we can set up the DMX client
    reader->Read(&frame);
    DMX* sender = new DMX();

    sender->init(frame.sacn_ip(), frame.universe(), frame.number_of_motors());

    // Process the incoming stream of frames
    do {

        debug("received frame {} for {}", frame_count, frame.creature_name());
        const std::string& frame_data = frame.frame();
        uint8_t buffer[frame.number_of_motors()];

        int i = 0;
        for (uint8_t byte : frame_data) {
            trace("byte {}: 0x{:02x}", i, byte);
            buffer[i++] = byte;
        }
        debug("frame transmitted");

        sender->send(buffer, frame.number_of_motors());

        // Increment the frame count
        frame_count++;
    } while (reader->Read(&frame));
    info("end of frames from client");

    delete sender;

    // Set the response
    response->set_frames_processed(frame_count);
    response->set_message(fmt::format("{} frames processed successfully", frame_count));

    return Status::OK;
}


