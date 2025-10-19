
#include <fmt/format.h>

#include "server/config.h"

#include "spdlog/spdlog.h"

#include "CooperativeAnimationScheduler.h"
#include "LegacyAnimationScheduler.h"
#include "SessionManager.h"
#include "exception/exception.h"
#include "model/Animation.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;

/**
 * Schedules an animation on a given creature
 *
 * This function routes to either the legacy bulk scheduler or the modern cooperative
 * scheduler based on the configuration setting. The cooperative scheduler supports
 * instant cancellation and interactive overrides, while the legacy scheduler is
 * preserved for backwards compatibility and testing.
 *
 * @param startingFrame the frame number to start the animation on
 * @param animation the animation to play
 * @param universe the universe to play the animation on
 *
 * @return the frame number of last frame of the animation (legacy), or session handle (cooperative)
 */
Result<framenum_t> scheduleAnimation(framenum_t startingFrame, const creatures::Animation &animation,
                                     universe_t universe) {

    // Check which scheduler to use
    auto schedulerType = config->getAnimationSchedulerType();

    if (schedulerType == Configuration::AnimationSchedulerType::Cooperative) {
        // Use the modern cooperative scheduler
        debug("Using cooperative animation scheduler for animation '{}'", animation.metadata.title);

        auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(startingFrame, animation, universe);
        if (!sessionResult.isSuccess()) {
            return Result<framenum_t>{sessionResult.getError().value()};
        }

        // Get the session and register it with SessionManager so it can be cancelled later
        auto session = sessionResult.getValue().value();
        sessionManager->registerSession(universe, session, false); // false = not a playlist

        // Calculate the last frame of the animation
        // Each animation frame takes (milliseconds_per_frame / EVENT_LOOP_PERIOD_MS) event loop frames
        framenum_t framesPerAnimFrame = animation.metadata.milliseconds_per_frame / EVENT_LOOP_PERIOD_MS;
        framenum_t lastFrame = startingFrame + framesPerAnimFrame * (animation.metadata.number_of_frames - 1);

        debug("Cooperative scheduler: animation '{}' will run from frame {} to {} ({} animation frames at {}ms each)",
              animation.metadata.title, startingFrame, lastFrame, animation.metadata.number_of_frames,
              animation.metadata.milliseconds_per_frame);

        return Result<framenum_t>{lastFrame};

    } else {
        // Use the legacy bulk scheduler
        debug("Using legacy animation scheduler for animation '{}'", animation.metadata.title);
        return LegacyAnimationScheduler::scheduleAnimation(startingFrame, animation, universe);
    }
}

} // namespace creatures
