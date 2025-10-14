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

    // Cancel any existing session on this universe
    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && it->second.currentSession) {
        debug("SessionManager: cancelling existing session on universe {} for new session", universe);
        it->second.currentSession->cancel();
        if (span) {
            span->setAttribute("cancelled_existing_session", true);
        }
    }

    // Register new session
    UniverseState state;
    state.currentSession = session;
    state.isPlaylist = isPlaylist;
    state.isInterrupted = false;
    universeStates_[universe] = state;

    info("SessionManager: registered new session on universe {} (playlist: {})", universe, isPlaylist);

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

    // TODO: Implement actual playlist resumption
    // For now, just clear the interrupted flag
    info("SessionManager: playlist resumption on universe {} (full implementation pending)", universe);
    it->second.isInterrupted = false;

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

} // namespace creatures
