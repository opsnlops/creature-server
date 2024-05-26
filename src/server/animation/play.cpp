
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/animation/player.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/metrics/counters.h"
#include "util/Result.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;


    /**
     * Fetch an animation from the database and then play it
     *
     * TODO: This shouldn't be in the database class. This is an artifact of the old gRPC code.
     *
     * @param animationId the id of the animation to play
     * @param universe which universe to play the animation on
     * @return a status message
     */
    Result<std::string> Database::playStoredAnimation(animationId_t animationId, universe_t universe) {

        debug("Playing a stored animation {} on universe {}", animationId, universe);

        framenum_t startingFrame = eventLoop->getNextFrameNumber() + ANIMATION_DELAY_FRAMES;
        framenum_t lastFrame;

        auto animationResult = db->getAnimation(animationId);
        if (!animationResult.isSuccess()) {
            auto error = animationResult.getError().value();
            auto errorMessage = fmt::format("Not able to play animation: {}", error.getMessage());
            warn(errorMessage);
            return Result<std::string>{error};
        }

        auto animation = animationResult.getValue().value();
        info("Playing animation {} on universe {}", animation.metadata.title, universe);

        auto playResult = scheduleAnimation(startingFrame, animation, universe);
        if (!playResult.isSuccess()) {
            auto error = playResult.getError().value();
            auto errorMessage = fmt::format("Not able to schedule animation: {}", error.getMessage());
            warn(errorMessage);
            return Result<std::string>{error};
        }

        // What was the last frame of the animation?
        lastFrame = playResult.getValue().value();
        auto okayMessage = fmt::format("✅ Animation scheduled from frame {} to {}", startingFrame, lastFrame);
        info(okayMessage);

        return Result<std::string>{okayMessage};
    }


}