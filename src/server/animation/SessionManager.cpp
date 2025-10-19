//
// SessionManager.cpp
// Manages active playback sessions and handles interrupts
//

#include "SessionManager.h"

#include "CooperativeAnimationScheduler.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
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
        } else {
            debug("SessionManager: existing session on universe {} already cancelled/finished, not cancelling again",
                  universe);
        }
    }

    // Register new session - preserve existing playlist state if present
    if (it != universeStates_.end()) {
        // Preserve existing playlist state (isPlaylist, playlistId, isInterrupted, etc.)
        it->second.currentSession = session;
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

    if (span) {
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
    auto sessionResult =
        CooperativeAnimationScheduler::scheduleAnimation(eventLoop->getNextFrameNumber(), interruptAnimation, universe);

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

    return true;
}

void SessionManager::cancelUniverse(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.currentSession) {
        info("SessionManager: cancelling all playback on universe {}", universe);
        it->second.currentSession->cancel();
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

        // Cancel the current session
        if (it->second.currentSession) {
            it->second.currentSession->cancel();
        }
    }
}

void SessionManager::startPlaylist(universe_t universe, const std::string &playlistId) {
    std::lock_guard<std::mutex> lock(mutex_);

    info("SessionManager: registering playlist start on universe {} (playlist: {})", universe, playlistId);

    // Create or update state for this universe
    UniverseState state;
    state.currentSession = nullptr; // No session yet, will be set when first animation plays
    state.isPlaylist = true;
    state.isInterrupted = false;
    state.isStopped = false;
    state.shouldResumePlaylist = false;
    state.playlistId = playlistId;

    universeStates_[universe] = state;
}

void SessionManager::clearCurrentSession(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        debug("SessionManager: clearing current session pointer for universe {} (preserving playlist state)", universe);
        it->second.currentSession = nullptr;
    }
}

} // namespace creatures
