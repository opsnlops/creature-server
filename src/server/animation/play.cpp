
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/animation/player.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/metrics/counters.h"
#include "exception/exception.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    grpc::Status CreatureServerImpl::PlayAnimation(grpc::ServerContext *context, const PlayAnimationRequest *request,
                                                   PlayAnimationResponse *response) {

        info("Playing an animation from a gRPC request");

        grpc::Status status;

        uint64_t startingFrame = eventLoop->getNextFrameNumber() + 500;
        uint64_t lastFrame;

        try {
            lastFrame = scheduleAnimation(startingFrame, request->creatureid(),request->animationid());
        }
        catch (const creatures::NotFoundException &e) {
            info("creature not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature id not found"));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while loading a creature on animation playback: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Data format exception while loading a creature on animation playback");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty creatureID was passed in while attempting to play an animation");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }
        catch( ... ) {
            warn("an unknown exception was thrown while attempting to load a creature to schedule an animation");
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "An unexpected error occurred while scheduling an animation",
                                  fmt::format("🤯️ An unhandled exception happened while trying to schedule an animation"));
            return status;
        }
        std::string okayMessage = fmt::format("✅ Animation scheduled from frame {} to {}", startingFrame, lastFrame);

        info(okayMessage);
        *response->mutable_status() = okayMessage;
        status = grpc::Status(grpc::StatusCode::OK,
                              okayMessage);
        
        return status;
    }

}