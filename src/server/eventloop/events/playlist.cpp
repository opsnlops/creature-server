

#include <algorithm>
#include <random>
#include <vector>

#include "spdlog/spdlog.h"

#include "model/PlaylistStatus.h"
#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/database.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;
extern std::shared_ptr<SessionManager> sessionManager;

PlaylistEvent::PlaylistEvent(framenum_t frameNumber_, universe_t universe_)
    : EventBase(frameNumber_), activeUniverse(universe_) {}

Result<framenum_t> PlaylistEvent::executeImpl() {

    auto span = observability->createOperationSpan("playlist_event.execute");
    span->setAttribute("active_universe", activeUniverse);

    debug("hello from a playlist event for universe {}", activeUniverse);
    metrics->incrementPlaylistsEventsProcessed();

    // Single source of truth: Check playlist state via SessionManager
    auto playlistState = sessionManager->getPlaylistState(activeUniverse);
    span->setAttribute("playlist_state", static_cast<int>(playlistState));

    switch (playlistState) {
    case PlaylistState::None:
    case PlaylistState::Stopped: {
        // Playlist was stopped (either never existed or explicitly stopped via regular play)
        std::string errorMessage =
            fmt::format("Playlist on universe {} is stopped or doesn't exist. Cleaning up.", activeUniverse);
        info(errorMessage);
        runningPlaylists->remove(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        span->setAttribute("reason", "stopped_or_none");
        span->setSuccess(); // Not an error, just cleanup
        return Result<framenum_t>{this->frameNumber};
    }

    case PlaylistState::Interrupted: {
        // Playlist is temporarily interrupted (will resume after interrupt animation finishes)
        info("Playlist on universe {} is interrupted, skipping scheduled event", activeUniverse);
        span->setAttribute("reason", "interrupted");
        span->setSuccess();
        return Result<framenum_t>{this->frameNumber};
    }

    case PlaylistState::Active:
        // Playlist is active, continue with normal scheduling logic below
        debug("Playlist on universe {} is active, continuing", activeUniverse);
        break;
    }

    // Additional check: Don't schedule if there's ANY animation currently playing
    // This prevents old queued PlaylistEvents from scheduling concurrent animations
    if (sessionManager->isPlaying(activeUniverse)) {
        info("Playlist on universe {} has active session, skipping this scheduled event (likely old queued event)",
             activeUniverse);
        span->setAttribute("reason", "active_session_present");
        span->setSuccess();
        return Result<framenum_t>{this->frameNumber};
    }

    // Go fetch the active playlist - check cache first
    if (!runningPlaylists->contains(activeUniverse)) {
        std::string errorMessage = fmt::format(
            "Playlist state is Active but runningPlaylists cache doesn't have entry for universe {}. Cleaning up.",
            activeUniverse);
        warn(errorMessage);
        sessionManager->stopPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        span->setAttribute("reason", "cache_missing");
        span->setSuccess();
        return Result<framenum_t>{this->frameNumber};
    }

    auto activePlaylistStatus = runningPlaylists->get(activeUniverse);
    debug("the active playlistStatus out of the cache is {}", activePlaylistStatus->playlist);

    // Go look this one up
    auto dbSpan = observability->createChildOperationSpan("music_event.db_lookup", span);
    if (dbSpan) {
        dbSpan->setAttribute("playlist.id", activePlaylistStatus->playlist);
    }
    auto playListResult = db->getPlaylist(activePlaylistStatus->playlist, dbSpan);
    if (!playListResult.isSuccess()) {
        std::string errorMessage = fmt::format("Playlist ID {} not found while in a playlist event. halting playback.",
                                               activePlaylistStatus->playlist);
        warn(errorMessage);
        runningPlaylists->remove(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        dbSpan->setError(errorMessage);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto playlist = playListResult.getValue().value();
    debug("playlist found. name: {}", playlist.name);
    dbSpan->setSuccess();

    /*
     * Brute force way to determine which animation to play
     *
     * This isn't the best way to do this, but the amount of memory needed is pretty small, really, and we've
     * got the RAM and CPU to burn.
     *
     * Create a list of every possible animation in the playlist. Add it the number of times that it's weighted,
     * and then pick a random one from the list of weighted animations.
     */

    std::vector<std::string> choices;
    for (const auto &playlistItem : playlist.items) {
        for (uint32_t i = 0; i < playlistItem.weight; i++) {
            choices.push_back(playlistItem.animation_id);
        }
        debug("added an animation to the list. {} now possible", choices.size());
    }

    // Pick a random animation the C++20 way
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, choices.size() - 1);
    size_t theChosenOne = dis(gen);

    auto chosenAnimation = choices[theChosenOne];
    debug("...and the chosen one is {}", chosenAnimation);
    span->setAttribute("chosen_animation", chosenAnimation);

    // Go get this animation
    auto animationSpan = observability->createChildOperationSpan("music_event.animation_lookup", span);
    if (animationSpan) {
        animationSpan->setAttribute("animation.id", chosenAnimation);
    }

    Result<Animation> animationResult = db->getAnimation(chosenAnimation, animationSpan);
    if (!animationResult.isSuccess()) {
        std::string errorMessage =
            fmt::format("Animation ID {} not found while in a playlist event. halting playback.", chosenAnimation);
        warn(errorMessage);
        if (animationSpan) {
            animationSpan->setError(errorMessage);
        }
        runningPlaylists->remove(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto animation = animationResult.getValue().value();

    if (animationSpan) {
        animationSpan->setAttribute("animation.title", animation.metadata.title);
        animationSpan->setSuccess();
    }

    // Schedule this animation
    auto scheduleResult = scheduleAnimation(eventLoop->getNextFrameNumber(), animation, activeUniverse);
    if (!scheduleResult.isSuccess()) {
        std::string errorMessage = fmt::format("Unable to schedule animation: {}. Halting playback.",
                                               scheduleResult.getError().value().getMessage());
        warn(errorMessage);
        runningPlaylists->remove(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto lastFrame = scheduleResult.getValue().value();

    debug("scheduled animation {} on universe {}. Last frame: {}", animation.metadata.title, activeUniverse, lastFrame);

    // Add another one of us to go again later
    auto nextEvent = std::make_shared<PlaylistEvent>(lastFrame + 1, activeUniverse);
    eventLoop->scheduleEvent(nextEvent);

    // Update the cache with the animation we're currently playing
    activePlaylistStatus->current_animation = chosenAnimation;
    runningPlaylists->put(activeUniverse, activePlaylistStatus);

    sendPlaylistUpdate(*activePlaylistStatus);

    debug("scheduled next event for frame {}. later!", lastFrame + 1);
    return Result<framenum_t>{lastFrame};
}

/**
 * Send a playlist update to all clients
 *
 * This can be used to let the clients know that the playlist has changed, and what the current animation is.
 *
 * @param playlistStatus the PlaylistStatus to send
 */
void PlaylistEvent::sendPlaylistUpdate(const PlaylistStatus &playlistStatus) {

    auto result = broadcastPlaylistStatusToAllClients(playlistStatus);
    if (!result.isSuccess()) {
        warn("Unable to broadcast playlist status to all clients: {}", result.getError().value().getMessage());
        // Just log. It's not a critical error.
    }
}

void PlaylistEvent::sendEmptyPlaylistUpdate(universe_t universe) {
    PlaylistStatus emptyStatus{};
    emptyStatus.universe = universe;
    emptyStatus.playlist = "";
    emptyStatus.playing = false;
    emptyStatus.current_animation = "";
    sendPlaylistUpdate(emptyStatus);
}

} // namespace creatures