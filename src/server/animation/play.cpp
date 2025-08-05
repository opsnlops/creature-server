
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/animation/player.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;

/**
 * Fetch an animation from the database and then play it
 *
 * TODO: This shouldn't be in the database class. This is an artifact of the old gRPC code.
 *
 * @param animationId the id of the animation to play
 * @param universe which universe to play the animation on
 * @return a status message
 */
Result<std::string> Database::playStoredAnimation(animationId_t animationId, universe_t universe,
                                                  std::shared_ptr<OperationSpan> parentSpan) {

    debug("Playing a stored animation {} on universe {}", animationId, universe);

    // Create a span for this playback operation
    auto playSpan = observability->createChildOperationSpan("Database.playStoredAnimation", parentSpan);
    if (playSpan) {
        playSpan->setAttribute("animation.id", animationId);
        playSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
    }

    framenum_t startingFrame = eventLoop->getNextFrameNumber() + ANIMATION_DELAY_FRAMES;
    framenum_t lastFrame;

    auto animationResult = db->getAnimation(animationId, playSpan);
    if (!animationResult.isSuccess()) {
        auto error = animationResult.getError().value();
        auto errorMessage = fmt::format("Not able to play animation: {}", error.getMessage());
        warn(errorMessage);
        if (playSpan) {
            playSpan->setError(errorMessage);
        }
        return Result<std::string>{error};
    }

    auto animation = animationResult.getValue().value();
    info("Playing animation {} on universe {}", animation.metadata.title, universe);

    auto playResult = scheduleAnimation(startingFrame, animation, universe);
    if (!playResult.isSuccess()) {
        auto error = playResult.getError().value();
        auto errorMessage = fmt::format("Not able to schedule animation: {}", error.getMessage());
        warn(errorMessage);
        if (playSpan) {
            playSpan->setError(errorMessage);
        }
        return Result<std::string>{error};
    }

    // What was the last frame of the animation?
    lastFrame = playResult.getValue().value();
    auto okayMessage = fmt::format("✅ Animation scheduled from frame {} to {}", startingFrame, lastFrame);
    info(okayMessage);

    if (playSpan) {
        playSpan->setAttribute("animation.title", animation.metadata.title);
        playSpan->setAttribute("animation.startFrame", static_cast<int64_t>(startingFrame));
        playSpan->setAttribute("animation.lastFrame", static_cast<int64_t>(lastFrame));
        playSpan->setSuccess();
    }

    return Result<std::string>{okayMessage};
}

} // namespace creatures