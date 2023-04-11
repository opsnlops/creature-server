

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/database.h"


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

    // Process the incoming stream of frames
    while (reader->Read(&frame)) {

        info("frame contents for {}:\n", frame.creature_name());
        const std::string& frame_data = frame.frame();

        for (unsigned char byte : frame_data) {
            info(" - 0x{:02x}", byte);
        }
        info("done");

        // Increment the frame count
        frame_count++;
    }
    info("end of frames from client");

    // Set the response
    response->set_frames_processed(frame_count);
    response->set_message(fmt::format("{} frames processed successfully", frame_count));

    return Status::OK;
}


