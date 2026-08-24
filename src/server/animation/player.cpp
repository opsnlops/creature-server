
#include <algorithm>
#include <fmt/format.h>

#include "server/config.h"

#include "spdlog/spdlog.h"

#include "CooperativeAnimationScheduler.h"
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
 * Uses the cooperative scheduler, which supports instant cancellation and
 * interactive overrides.
 *
 * @param startingFrame the frame number to start the animation on
 * @param animation the animation to play
 * @param universe the universe to play the animation on
 *
 * @return the frame number of last frame of the animation
 */
Result<framenum_t> scheduleAnimation(framenum_t startingFrame, const creatures::Animation &animation,
                                     universe_t universe, creatures::runtime::ActivityReason reason,
                                     std::optional<uint64_t> expectedPlaylistGeneration) {

    if (animation.metadata.milliseconds_per_frame == 0 || animation.metadata.number_of_frames == 0) {
        std::string errorMessage =
            fmt::format("Invalid animation timing data for '{}' (ms_per_frame={}, frames={})", animation.metadata.title,
                        animation.metadata.milliseconds_per_frame, animation.metadata.number_of_frames);
        warn(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    if (!config) {
        std::string errorMessage = "Animation scheduler unavailable: configuration missing";
        warn(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    if (!sessionManager) {
        std::string errorMessage = "Animation scheduler unavailable: session manager missing";
        warn(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(startingFrame, animation, universe, reason,
                                                                          false, {}, expectedPlaylistGeneration);
    if (!sessionResult.isSuccess()) {
        return Result<framenum_t>{sessionResult.getError().value()};
    }

    // The session was already adopted (registered) inside scheduleAnimation, atomically
    // with cancelling any conflicting sessions — see issue #62.
    auto session = sessionResult.getValue().value();

    // Calculate the last frame of the animation
    // Each animation frame takes (milliseconds_per_frame / EVENT_LOOP_PERIOD_MS) event loop frames
    framenum_t framesPerAnimFrame = std::max<framenum_t>(
        1, static_cast<framenum_t>(animation.metadata.milliseconds_per_frame / EVENT_LOOP_PERIOD_MS));
    framenum_t lastFrame = startingFrame + framesPerAnimFrame * (animation.metadata.number_of_frames - 1);

    debug("Cooperative scheduler: animation '{}' will run from frame {} to {} ({} animation frames at {}ms each)",
          animation.metadata.title, startingFrame, lastFrame, animation.metadata.number_of_frames,
          animation.metadata.milliseconds_per_frame);

    return Result<framenum_t>{lastFrame};
}

} // namespace creatures
