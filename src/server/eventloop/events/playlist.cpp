
#include <algorithm>
#include <random>
#include <stdexcept>
#include <unordered_set>
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
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"

#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include "util/cache.h"
#include "util/websocketUtils.h"

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

PlaylistEvent::PlaylistEvent(framenum_t frameNumber_, universe_t universe_)
    : EventBase(frameNumber_), activeUniverse(universe_) {}

Result<framenum_t> PlaylistEvent::executeImpl() {

    auto span = observability ? observability->createOperationSpan("playlist_event.execute") : nullptr;
    if (span) {
        span->setAttribute("active_universe", activeUniverse);
    }

    debug("hello from a playlist event for universe {}", activeUniverse);
    metrics->incrementPlaylistsEventsProcessed();

    // Single source of truth: Check playlist state via SessionManager
    auto playlistState = sessionManager->getPlaylistState(activeUniverse);
    if (span) {
        span->setAttribute("playlist_state", static_cast<int>(playlistState));
    }

    switch (playlistState) {
    case PlaylistState::None:
    case PlaylistState::Stopped: {
        // Playlist was stopped (either never existed or explicitly stopped via regular play)
        std::string errorMessage =
            fmt::format("Playlist on universe {} is stopped or doesn't exist. Cleaning up.", activeUniverse);
        info(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (span) {
            span->setAttribute("reason", "stopped_or_none");
            span->setSuccess(); // Not an error, just cleanup
        }
        return Result<framenum_t>{this->frameNumber};
    }

    case PlaylistState::Interrupted: {
        // Playlist is temporarily interrupted (will resume after interrupt animation finishes)
        info("Playlist on universe {} is interrupted, skipping scheduled event", activeUniverse);
        if (span) {
            span->setAttribute("reason", "interrupted");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

    case PlaylistState::Active:
        // Playlist is active, continue with normal scheduling logic below
        debug("Playlist on universe {} is active, continuing", activeUniverse);
        break;
    }

    // Additional check: Don't schedule if there's ANY animation currently playing
    // This prevents old queued PlaylistEvents from scheduling concurrent animations
    // Allow idle-only sessions; we only skip if a non-idle session is active.
    if (sessionManager->hasActiveNonIdleSession(activeUniverse)) {
        info("Playlist on universe {} has active non-idle session, skipping scheduled event", activeUniverse);
        if (span) {
            span->setAttribute("reason", "non_idle_session_present");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

    // Go fetch the active playlist snapshot
    auto activePlaylistStatusOpt = sessionManager->getPlaylistStatus(activeUniverse);
    if (!activePlaylistStatusOpt || activePlaylistStatusOpt->playlist.empty()) {
        std::string errorMessage = fmt::format(
            "Playlist state is Active but no playlist snapshot exists for universe {}. Cleaning up.", activeUniverse);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (span) {
            span->setAttribute("reason", "cache_missing");
            span->setSuccess();
        }
        return Result<framenum_t>{this->frameNumber};
    }

    PlaylistStatus activePlaylistStatus = *activePlaylistStatusOpt;
    debug("the active playlistStatus snapshot is {}", activePlaylistStatus.playlist);

    // Go look this one up
    auto dbSpan = observability ? observability->createChildOperationSpan("music_event.db_lookup", span) : nullptr;
    if (dbSpan) {
        dbSpan->setAttribute("playlist.id", activePlaylistStatus.playlist);
    }
    auto playListResult = db->getPlaylist(activePlaylistStatus.playlist, dbSpan);
    if (!playListResult.isSuccess()) {
        std::string errorMessage = fmt::format("Playlist ID {} not found while in a playlist event. halting playback.",
                                               activePlaylistStatus.playlist);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        if (dbSpan) {
            dbSpan->setError(errorMessage);
        }
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto playlist = playListResult.getValue().value();
    debug("playlist found. name: {}", playlist.name);
    if (dbSpan) {
        dbSpan->setSuccess();
    }

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
    if (choices.empty()) {
        std::string errorMessage =
            fmt::format("Playlist {} has no animations to schedule. Halting playlist playback.", playlist.id);
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }

    // Pick a random animation the C++20 way
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dis(0, choices.size() - 1);
    size_t theChosenOne = dis(gen);

    auto chosenAnimation = choices[theChosenOne];
    debug("...and the chosen one is {}", chosenAnimation);
    if (span) {
        span->setAttribute("chosen_animation", chosenAnimation);
    }

    // Go get this animation
    auto animationSpan = observability->createChildOperationSpan("music_event.animation_lookup", span);
    if (animationSpan) {
        animationSpan->setAttribute("animation.id", chosenAnimation);
    }

    Result<Animation> animationResult = db->getAnimation(chosenAnimation, animationSpan);
    if (!animationResult.isSuccess()) {
        std::string errorMessage = fmt::format(
            "Animation ID {} not found while in a playlist event. Halting playlist playback.", chosenAnimation);
        warn(errorMessage);
        if (animationSpan) {
            animationSpan->setError(errorMessage);
        }
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto animation = animationResult.getValue().value();
    std::unordered_set<creatureId_t> involvedCreatures;
    involvedCreatures.reserve(animation.tracks.size());
    for (const auto &track : animation.tracks) {
        if (!track.creature_id.empty()) {
            involvedCreatures.insert(track.creature_id);
        }
    }
    if (span) {
        span->setAttribute("playlist.animation_creature_count", static_cast<int64_t>(involvedCreatures.size()));
    }

    if (animationSpan) {
        animationSpan->setAttribute("animation.title", animation.metadata.title);
        animationSpan->setSuccess();
    }

    // Schedule this animation
    auto scheduleResult = scheduleAnimation(eventLoop->getNextFrameNumber(), animation, activeUniverse,
                                            creatures::runtime::ActivityReason::Playlist);
    if (!scheduleResult.isSuccess()) {
        std::string errorMessage = fmt::format("Unable to schedule animation: {}. Halting playlist playback.",
                                               scheduleResult.getError().value().getMessage());
        warn(errorMessage);
        sessionManager->clearPlaylist(activeUniverse);
        sendEmptyPlaylistUpdate(activeUniverse);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
    }
    auto lastFrame = scheduleResult.getValue().value();

    debug("scheduled animation {} on universe {}. Last frame: {}", animation.metadata.title, activeUniverse, lastFrame);

    // Add another one of us to go again later
    auto nextEvent = std::make_shared<PlaylistEvent>(lastFrame + 1, activeUniverse);
    eventLoop->scheduleEvent(nextEvent);

    // Update the cache with the animation we're currently playing
    activePlaylistStatus.current_animation = chosenAnimation;
    sessionManager->setPlaylistStatus(activeUniverse, activePlaylistStatus);

    sendPlaylistUpdate(activePlaylistStatus);

    // Start idle loops for creatures on this universe not involved in the playlist animation.
    if (creatureUniverseMap) {
        auto allCreatures = creatureUniverseMap->getAllKeys();
        size_t idleCandidates = 0;
        for (const auto &creatureId : allCreatures) {
            if (involvedCreatures.contains(creatureId)) {
                continue;
            }
            try {
                if (auto universePtr = creatureUniverseMap->get(creatureId);
                    !universePtr || *universePtr != activeUniverse) {
                    continue;
                }
            } catch (const std::out_of_range &) {
                // The universe map can change between the snapshot and lookup; ignore stale entries.
                continue;
            }
            idleCandidates++;
            creatures::ws::CreatureService::startIdleIfNeeded(creatureId, span);
        }
        if (span) {
            span->setAttribute("playlist.idle_candidates", static_cast<int64_t>(idleCandidates));
        }
    }

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

    if (const auto result = broadcastPlaylistStatusToAllClients(playlistStatus); !result.isSuccess()) {
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
