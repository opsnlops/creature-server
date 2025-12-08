//
// SessionManager.cpp
// Manages active playback sessions and handles interrupts
//

#include "SessionManager.h"

#include "CooperativeAnimationScheduler.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;

void SessionManager::registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist) {
    auto span = observability->createOperationSpan("SessionManager.registerSession");
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("is_playlist", isPlaylist);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    bool playlistContext = isPlaylist;

    // Cancel any existing session on this universe (but only if it's still active)
    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.currentSession) {
        // Only cancel if the session is still running (not already cancelled or finished)
        if (!it->second.currentSession->isCancelled()) {
            debug("SessionManager: cancelling existing session on universe {} for new session", universe);
            it->second.currentSession->cancel();
            if (span) {
                span->setAttribute("cancelled_existing_session", true);
            }
            std::vector<creatureId_t> creatureIds;
            for (const auto &trackState : it->second.currentSession->getTrackStates()) {
                creatureIds.push_back(trackState.creatureId);
            }
            creatures::ws::CreatureService::setActivityState(creatureIds, it->second.currentSession->getAnimation().id,
                                                             creatures::runtime::ActivityReason::Cancelled,
                                                             creatures::runtime::ActivityState::Stopped,
                                                             it->second.currentSession->getSessionId());
        } else {
            debug("SessionManager: existing session on universe {} already cancelled/finished, not cancelling again",
                  universe);
        }
    }

    // Register new session - preserve existing playlist state if present
    if (it != universeStates_.end()) {
        // Preserve existing playlist state (isPlaylist, playlistId, isInterrupted, etc.)
        it->second.currentSession = session;
        playlistContext = playlistContext || it->second.isPlaylist;
        // Only update isPlaylist if we're explicitly setting it to true (starting a new playlist)
        if (isPlaylist) {
            it->second.isPlaylist = true;
        }
        debug("SessionManager: updated session on universe {} (playlist: {})", universe, it->second.isPlaylist);
    } else {
        // No existing state, create new
        UniverseState state;
        state.currentSession = session;
        state.isPlaylist = isPlaylist;
        state.isInterrupted = false;
        state.isStopped = false;
        universeStates_[universe] = state;
        info("SessionManager: registered new session on universe {} (playlist: {})", universe, isPlaylist);
    }

    if (playlistContext) {
        session->setActivityReason(creatures::runtime::ActivityReason::Playlist);
    }

    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
}

Result<std::shared_ptr<PlaybackSession>>
SessionManager::interrupt(universe_t universe, const Animation &interruptAnimation, bool shouldResumePlaylist) {
    auto span = observability->createOperationSpan("SessionManager.interrupt");
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("interrupt.animation_id", interruptAnimation.id);
        span->setAttribute("interrupt.animation_title", interruptAnimation.metadata.title);
        span->setAttribute("interrupt.should_resume_playlist", shouldResumePlaylist);
    }

    bool interruptedPlaylist = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if there's something playing
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end() && it->second.currentSession) {
            info("SessionManager: interrupting playback on universe {} with animation '{}'", universe,
                 interruptAnimation.metadata.title);

            // Cancel current session
            it->second.currentSession->cancel();
            // Mark activity as cancelled for involved creatures
            std::vector<creatureId_t> creatureIds;
            for (const auto &trackState : it->second.currentSession->getTrackStates()) {
                creatureIds.push_back(trackState.creatureId);
            }
            creatures::ws::CreatureService::setActivityState(creatureIds, it->second.currentSession->getAnimation().id,
                                                             creatures::runtime::ActivityReason::Cancelled,
                                                             creatures::runtime::ActivityState::Stopped,
                                                             it->second.currentSession->getSessionId());

            // Mark as interrupted if it was a playlist
            if (it->second.isPlaylist) {
                it->second.isInterrupted = true;
                it->second.shouldResumePlaylist = shouldResumePlaylist;
                interruptedPlaylist = true;
                info("SessionManager: marked playlist on universe {} as interrupted (resume: {})", universe,
                     shouldResumePlaylist);
            }
        }
    }

    if (span) {
        span->setAttribute("interrupted_playlist", interruptedPlaylist);
    }

    // Schedule the interrupt animation using cooperative scheduler
    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(
        eventLoop->getNextFrameNumber(), interruptAnimation, universe, creatures::runtime::ActivityReason::AdHoc);

    if (!sessionResult.isSuccess()) {
        error("SessionManager: failed to schedule interrupt animation: {}", sessionResult.getError()->getMessage());
        if (span) {
            span->setError(sessionResult.getError()->getMessage());
        }
        return sessionResult;
    }

    auto session = sessionResult.getValue().value();

    // Register the interrupt session (but NOT as a playlist)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end()) {
            // Keep the playlist state but update current session
            it->second.currentSession = session;
            // isInterrupted remains true if it was set
        } else {
            // No previous state, create new
            UniverseState state;
            state.currentSession = session;
            state.isPlaylist = false;
            state.isInterrupted = false;
            universeStates_[universe] = state;
        }
    }

    info("SessionManager: interrupt animation '{}' scheduled on universe {}", interruptAnimation.metadata.title,
         universe);

    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }

    return Result<std::shared_ptr<PlaybackSession>>{session};
}

bool SessionManager::resumePlaylist(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || !it->second.isInterrupted) {
        debug("SessionManager: no interrupted playlist to resume on universe {}", universe);
        return false;
    }

    // Clear the interrupted flag so PlaylistEvents can schedule animations again
    info("SessionManager: resuming playlist on universe {}", universe);
    it->second.isInterrupted = false;
    it->second.shouldResumePlaylist = false;
    if (it->second.playlistStatus) {
        it->second.playlistStatus->playing = true;
    }

    return true;
}

void SessionManager::cancelUniverse(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.currentSession) {
        info("SessionManager: cancelling all playback on universe {}", universe);
        it->second.currentSession->cancel();
        // Mark activity as cancelled for involved creatures
        std::vector<creatureId_t> creatureIds;
        for (const auto &trackState : it->second.currentSession->getTrackStates()) {
            creatureIds.push_back(trackState.creatureId);
        }
        creatures::ws::CreatureService::setActivityState(
            creatureIds, it->second.currentSession->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
            creatures::runtime::ActivityState::Stopped, it->second.currentSession->getSessionId());
        universeStates_.erase(it);
    }
}

std::shared_ptr<PlaybackSession> SessionManager::getCurrentSession(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        return it->second.currentSession;
    }

    return nullptr;
}

bool SessionManager::isPlaying(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.currentSession) {
        return !it->second.currentSession->isCancelled();
    }

    return false;
}

bool SessionManager::hasInterruptedPlaylist(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    return (it != universeStates_.end() && it->second.isInterrupted);
}

PlaylistState SessionManager::getPlaylistState(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return PlaylistState::None;
    }

    const auto &state = it->second;

    // Not a playlist at all
    if (!state.isPlaylist) {
        return PlaylistState::None;
    }

    // Explicitly stopped
    if (state.isStopped) {
        return PlaylistState::Stopped;
    }

    // Temporarily interrupted
    if (state.isInterrupted) {
        return PlaylistState::Interrupted;
    }

    // Active and playing
    return PlaylistState::Active;
}

void SessionManager::stopPlaylist(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.isPlaylist) {
        info("SessionManager: stopping playlist on universe {}", universe);
        it->second.isStopped = true;
        it->second.isInterrupted = false;
        it->second.shouldResumePlaylist = false;
        if (it->second.playlistStatus) {
            it->second.playlistStatus->playing = false;
            it->second.playlistStatus->current_animation.clear();
        }

        // Cancel the current session
        if (it->second.currentSession) {
            it->second.currentSession->cancel();
            std::vector<creatureId_t> creatureIds;
            for (const auto &trackState : it->second.currentSession->getTrackStates()) {
                creatureIds.push_back(trackState.creatureId);
            }
            creatures::ws::CreatureService::setActivityState(creatureIds, it->second.currentSession->getAnimation().id,
                                                             creatures::runtime::ActivityReason::Cancelled,
                                                             creatures::runtime::ActivityState::Stopped,
                                                             it->second.currentSession->getSessionId());
        }
    }
}

void SessionManager::startPlaylist(universe_t universe, const std::string &playlistId) {
    std::lock_guard<std::mutex> lock(mutex_);

    info("SessionManager: registering playlist start on universe {} (playlist: {})", universe, playlistId);

    auto &state = universeStates_[universe];
    state.currentSession = nullptr; // No session yet, will be set when first animation plays
    state.isPlaylist = true;
    state.isInterrupted = false;
    state.isStopped = false;
    state.shouldResumePlaylist = false;
    state.playlistId = playlistId;
    if (!state.playlistStatus) {
        state.playlistStatus = PlaylistStatus{};
        state.playlistStatus->universe = universe;
    }
    state.playlistStatus->playlist = playlistId;
    state.playlistStatus->playing = true;
}

void SessionManager::clearCurrentSession(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        debug("SessionManager: clearing current session pointer for universe {} (preserving playlist state)", universe);
        it->second.currentSession = nullptr;
    }
}

void SessionManager::setPlaylistStatus(universe_t universe, const PlaylistStatus &status) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto &state = universeStates_[universe];
    if (!state.playlistStatus) {
        state.playlistStatus = PlaylistStatus{};
    }
    state.playlistStatus = status;
    state.playlistStatus->universe = universe;
    state.playlistId = status.playlist;
    state.isPlaylist = !status.playlist.empty();
    state.isStopped = !status.playing && state.isPlaylist;
}

std::optional<PlaylistStatus> SessionManager::getPlaylistStatus(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || !it->second.playlistStatus) {
        return std::nullopt;
    }
    PlaylistStatus snapshot = *it->second.playlistStatus;
    snapshot.universe = universe;
    return snapshot;
}

std::vector<PlaylistStatus> SessionManager::getAllPlaylistStatuses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PlaylistStatus> statuses;
    statuses.reserve(universeStates_.size());
    for (const auto &[universe, state] : universeStates_) {
        if (state.playlistStatus) {
            PlaylistStatus snapshot = *state.playlistStatus;
            snapshot.universe = universe;
            statuses.push_back(snapshot);
        }
    }
    return statuses;
}

void SessionManager::clearPlaylist(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return;
    }
    it->second.isPlaylist = false;
    it->second.isInterrupted = false;
    it->second.isStopped = false;
    it->second.shouldResumePlaylist = false;
    it->second.playlistId.clear();
    it->second.playlistStatus.reset();

    if (!it->second.currentSession) {
        universeStates_.erase(it);
    }
}

bool SessionManager::updatePlaylistCurrentAnimation(universe_t universe, const std::string &animationId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || !it->second.playlistStatus) {
        return false;
    }
    it->second.playlistStatus->current_animation = animationId;
    it->second.playlistStatus->playing = true;
    it->second.playlistStatus->universe = universe;
    return true;
}

} // namespace creatures
