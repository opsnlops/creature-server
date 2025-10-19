
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "model/PlaylistStatus.h"
#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/websocketUtils.h"

#include "server/namespace-stuffs.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;
extern std::shared_ptr<SessionManager> sessionManager;
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

    // If using cooperative scheduler, cancel any existing playback on this universe first
    // This prevents concurrent animations from running and conflicting with each other
    if (config->getAnimationSchedulerType() == Configuration::AnimationSchedulerType::Cooperative) {
        auto existingSession = sessionManager->getCurrentSession(universe);
        if (existingSession && !existingSession->isCancelled()) {
            info("Cooperative scheduler: cancelling existing playback on universe {} before starting new animation",
                 universe);
            existingSession->cancel();
            if (playSpan) {
                playSpan->setAttribute("cancelled_existing_session", true);
            }
        }

        // Stop any running playlist on this universe using the single source of truth
        // This marks the playlist as stopped in SessionManager, making future PlaylistEvents exit cleanly
        auto playlistState = sessionManager->getPlaylistState(universe);
        if (playlistState == PlaylistState::Active || playlistState == PlaylistState::Interrupted) {
            info("Stopping playlist on universe {} to play single animation", universe);
            sessionManager->stopPlaylist(universe);
            runningPlaylists->remove(universe);

            // Notify clients that playlist has stopped
            PlaylistStatus emptyStatus{};
            emptyStatus.universe = universe;
            emptyStatus.playlist = "";
            emptyStatus.playing = false;
            emptyStatus.current_animation = "";
            auto broadcastResult = broadcastPlaylistStatusToAllClients(emptyStatus);
            if (!broadcastResult.isSuccess()) {
                warn("Failed to broadcast playlist stop: {}", broadcastResult.getError()->getMessage());
            }

            if (playSpan) {
                playSpan->setAttribute("stopped_playlist", true);
            }
        }
    }

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