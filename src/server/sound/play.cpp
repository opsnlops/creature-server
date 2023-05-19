

#include "server/config.h"

#include <string>

#include "spdlog/spdlog.h"

#include "messaging/server.pb.h"
#include "server/creature-server.h"
#include "exception/exception.h"

#include "server/eventloop/events/types.h"


#include <fmt/format.h>

#include <grpcpp/grpcpp.h>

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

using creatures::MusicEvent;

using server::PlaySoundRequest;
using server::PlaySoundResponse;

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;

    grpc::Status CreatureServerImpl::PlaySound(grpc::ServerContext *context, const PlaySoundRequest *request,
                                               PlaySoundResponse *response) {

        info("Playing a sound via gPRC request");

        std::string soundFileName = MusicEvent::getSoundFileLocation() + "/" + request->filename();
        debug("using sound file name: {}", soundFileName);

        uint64_t frameNumber = eventLoop->getNextFrameNumber();


        // Create the event and schedule it
        auto playEvent = std::make_shared<MusicEvent>(frameNumber, soundFileName);
        eventLoop->scheduleEvent(playEvent);

        debug("scheduled sound to play on frame {}", frameNumber);

        response->set_message(fmt::format("Scheduled {} for frame {}", soundFileName, frameNumber));

        return grpc::Status::OK;
    }

}