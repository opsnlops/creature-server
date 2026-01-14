//
// SessionManager.cpp
// Manages active playback sessions and handles interrupts
//

#include "SessionManager.h"

#include "CooperativeAnimationScheduler.h"
#include "exception/exception.h"
#include "server/creature-server.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/runtime/Activity.h"
#include "server/ws/service/CreatureService.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"
#include <algorithm>
#include <unordered_set>

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;

namespace {

std::unordered_set<creatureId_t> collectCreatureIds(const std::shared_ptr<PlaybackSession> &session) {
    std::unordered_set<creatureId_t> creatureIds;
    if (!session) {
        return creatureIds;
    }
    for (const auto &trackState : session->getTrackStates()) {
        creatureIds.insert(trackState.creatureId);
    }
    return creatureIds;
}

bool sessionHasCreature(const std::shared_ptr<PlaybackSession> &session, const creatureId_t &creatureId) {
    if (!session) {
        return false;
    }
    for (const auto &trackState : session->getTrackStates()) {
        if (trackState.creatureId == creatureId) {
            return true;
        }
    }
    return false;
}

bool overlaps(const std::unordered_set<creatureId_t> &lhs, const std::shared_ptr<PlaybackSession> &session) {
    auto rhs = collectCreatureIds(session);
    for (const auto &id : lhs) {
        if (rhs.count(id) > 0) {
            return true;
        }
    }
    return false;
}

void cancelSessionAndMarkActivity(const std::shared_ptr<PlaybackSession> &session) {
    if (!session) {
        return;
    }
    if (session->getActivityReason() == creatures::runtime::ActivityReason::Idle) {
        std::vector<creatureId_t> idleCreatures;
        idleCreatures.reserve(session->getTrackStates().size());
        for (const auto &trackState : session->getTrackStates()) {
            idleCreatures.push_back(trackState.creatureId);
        }
        creatures::ws::CreatureService::incrementIdleStopped(idleCreatures);
    }

    session->cancel();
    session->markCancellationNotified();
    std::vector<creatureId_t> creatureIds;
    creatureIds.reserve(session->getTrackStates().size());
    for (const auto &trackState : session->getTrackStates()) {
        creatureIds.push_back(trackState.creatureId);
    }
    creatures::ws::CreatureService::setActivityState(
        creatureIds, session->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
        creatures::runtime::ActivityState::Stopped, session->getSessionId());
}

void scheduleImmediateTeardown(const std::shared_ptr<PlaybackSession> &session) {
    if (!session || !eventLoop) {
        return;
    }
    auto teardownRunner = std::make_shared<PlaybackRunnerEvent>(eventLoop->getNextFrameNumber(), session);
    eventLoop->scheduleEvent(teardownRunner);
}

} // namespace

void SessionManager::registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist) {
    if (!session) {
        warn("SessionManager: attempted to register null session on universe {}", universe);
        return;
    }

    auto span = observability ? observability->createOperationSpan("SessionManager.registerSession") : nullptr;
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("is_playlist", isPlaylist);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto newCreatures = collectCreatureIds(session);

    // Cancel overlapping sessions on this universe (last request wins per creature)
    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        std::vector<std::shared_ptr<PlaybackSession>> survivors;
        size_t cancelled = 0;
        survivors.reserve(it->second.activeSessions.size());
        for (auto &existing : it->second.activeSessions) {
            if (!existing) {
                continue;
            }
            if (overlaps(newCreatures, existing) && !existing->isCancelled()) {
                debug("SessionManager: cancelling overlapping session on universe {} for new session", universe);
                cancelSessionAndMarkActivity(existing);
                cancelled++;
            } else {
                survivors.push_back(existing);
            }
        }
        it->second.activeSessions.swap(survivors);
        if (span && cancelled > 0) {
            span->setAttribute("cancelled_existing_sessions", static_cast<int64_t>(cancelled));
        }
    }

    // Register new session - preserve existing playlist state if present
    if (it != universeStates_.end()) {
        // Preserve existing playlist state (isPlaylist, playlistId, isInterrupted, etc.)
        it->second.activeSessions.push_back(session);
        // Only update isPlaylist if we're explicitly setting it to true (starting a new playlist)
        if (isPlaylist) {
            it->second.isPlaylist = true;
        }
        debug("SessionManager: updated session on universe {} (playlist: {}, active_sessions: {})", universe,
              it->second.isPlaylist, it->second.activeSessions.size());
    } else {
        // No existing state, create new
        UniverseState state;
        state.activeSessions.push_back(session);
        state.isPlaylist = isPlaylist;
        state.isInterrupted = false;
        state.isStopped = false;
        universeStates_[universe] = state;
        info("SessionManager: registered new session on universe {} (playlist: {})", universe, isPlaylist);
    }

    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
}

Result<std::shared_ptr<PlaybackSession>>
SessionManager::interrupt(universe_t universe, const Animation &interruptAnimation, bool shouldResumePlaylist) {
    auto span = observability ? observability->createOperationSpan("SessionManager.interrupt") : nullptr;
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("interrupt.animation_id", interruptAnimation.id);
        span->setAttribute("interrupt.animation_title", interruptAnimation.metadata.title);
        span->setAttribute("interrupt.should_resume_playlist", shouldResumePlaylist);
    }

    if (!eventLoop) {
        std::string errorMessage = "SessionManager: event loop unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    bool interruptedPlaylist = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Check if there's something playing
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end()) {
            if (!it->second.activeSessions.empty()) {
                info("SessionManager: interrupting playback on universe {} with animation '{}'", universe,
                     interruptAnimation.metadata.title);
            }

            for (auto &existing : it->second.activeSessions) {
                if (existing && !existing->isCancelled()) {
                    cancelSessionAndMarkActivity(existing);
                }
            }
            it->second.activeSessions.clear();

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
            // Keep the playlist state but update current sessions
            it->second.activeSessions.push_back(session);
            // isInterrupted remains true if it was set
        } else {
            // No previous state, create new
            UniverseState state;
            state.activeSessions.push_back(session);
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

Result<std::shared_ptr<PlaybackSession>> SessionManager::interruptIdleOnly(universe_t universe,
                                                                           const Animation &interruptAnimation,
                                                                           const creatureId_t &creatureId) {
    auto span = observability ? observability->createOperationSpan("SessionManager.interruptIdleOnly") : nullptr;
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("interrupt.animation_id", interruptAnimation.id);
        span->setAttribute("interrupt.animation_title", interruptAnimation.metadata.title);
        span->setAttribute("creature.id", creatureId);
    }

    if (!eventLoop) {
        std::string errorMessage = "SessionManager: event loop unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    std::vector<std::shared_ptr<PlaybackSession>> cancelledSessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end()) {
            for (const auto &existing : it->second.activeSessions) {
                if (!existing || existing->isCancelled()) {
                    continue;
                }
                if (existing->getActivityReason() == creatures::runtime::ActivityReason::Idle) {
                    continue;
                }
                if (sessionHasCreature(existing, creatureId)) {
                    std::string errorMessage = "Creature " + creatureId + " already has an active non-idle session";
                    if (span) {
                        span->setError(errorMessage);
                    }
                    return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::Conflict, errorMessage)};
                }
            }

            std::vector<std::shared_ptr<PlaybackSession>> survivors;
            survivors.reserve(it->second.activeSessions.size());
            for (const auto &existing : it->second.activeSessions) {
                if (existing && !existing->isCancelled() &&
                    existing->getActivityReason() == creatures::runtime::ActivityReason::Idle &&
                    sessionHasCreature(existing, creatureId)) {
                    debug("SessionManager: cancelling idle session on universe {} for creature {} (ad-hoc)", universe,
                          creatureId);
                    cancelSessionAndMarkActivity(existing);
                    cancelledSessions.push_back(existing);
                } else {
                    survivors.push_back(existing);
                }
            }
            it->second.activeSessions.swap(survivors);
        }
    }

    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(
        eventLoop->getNextFrameNumber(), interruptAnimation, universe, creatures::runtime::ActivityReason::AdHoc);

    if (!sessionResult.isSuccess()) {
        for (const auto &cancelledSession : cancelledSessions) {
            scheduleImmediateTeardown(cancelledSession);
        }
        if (span) {
            span->setError(sessionResult.getError()->getMessage());
        }
        return sessionResult;
    }

    auto session = sessionResult.getValue().value();
    registerSession(universe, session, false);

    for (const auto &cancelledSession : cancelledSessions) {
        scheduleImmediateTeardown(cancelledSession);
    }

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
    if (it != universeStates_.end()) {
        if (!it->second.activeSessions.empty()) {
            info("SessionManager: cancelling all playback on universe {}", universe);
        }
        for (auto &session : it->second.activeSessions) {
            if (session) {
                cancelSessionAndMarkActivity(session);
            }
        }
        universeStates_.erase(it);
    }
}

std::shared_ptr<PlaybackSession> SessionManager::getCurrentSession(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() && !it->second.activeSessions.empty()) {
        for (auto rit = it->second.activeSessions.rbegin(); rit != it->second.activeSessions.rend(); ++rit) {
            if (*rit && !(*rit)->isCancelled()) {
                return *rit;
            }
        }
    }

    return nullptr;
}

void SessionManager::cancelSessionsForCreatures(universe_t universe, const std::vector<creatureId_t> &creatureIds) {
    std::unordered_set<creatureId_t> toCancel{creatureIds.begin(), creatureIds.end()};
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return;
    }

    std::vector<std::shared_ptr<PlaybackSession>> survivors;
    survivors.reserve(it->second.activeSessions.size());
    for (auto &session : it->second.activeSessions) {
        if (session && overlaps(toCancel, session) && !session->isCancelled()) {
            debug("SessionManager: cancelling session on universe {} for creature-specific request", universe);
            cancelSessionAndMarkActivity(session);
        } else {
            survivors.push_back(session);
        }
    }
    it->second.activeSessions.swap(survivors);
}

std::vector<std::shared_ptr<PlaybackSession>> SessionManager::getActiveSessions(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return {};
    }
    return it->second.activeSessions;
}

bool SessionManager::hasActiveSessionForCreature(universe_t universe, const creatureId_t &creatureId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return false;
    }

    for (const auto &session : it->second.activeSessions) {
        if (session && !session->isCancelled() && sessionHasCreature(session, creatureId)) {
            return true;
        }
    }
    return false;
}

bool SessionManager::cancelIdleSessionForCreature(universe_t universe, const creatureId_t &creatureId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return false;
    }

    bool cancelled = false;
    std::vector<std::shared_ptr<PlaybackSession>> survivors;
    survivors.reserve(it->second.activeSessions.size());
    for (auto &session : it->second.activeSessions) {
        if (session && !session->isCancelled() &&
            session->getActivityReason() == creatures::runtime::ActivityReason::Idle &&
            sessionHasCreature(session, creatureId)) {
            debug("SessionManager: cancelling idle session on universe {} for creature {}", universe, creatureId);
            cancelSessionAndMarkActivity(session);
            scheduleImmediateTeardown(session);
            cancelled = true;
        } else {
            survivors.push_back(session);
        }
    }
    it->second.activeSessions.swap(survivors);
    return cancelled;
}

bool SessionManager::isPlaying(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        return std::any_of(
            it->second.activeSessions.begin(), it->second.activeSessions.end(),
            [](const std::shared_ptr<PlaybackSession> &session) { return session && !session->isCancelled(); });
    }

    return false;
}

bool SessionManager::hasActiveNonIdleSession(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return false;
    }

    for (const auto &session : it->second.activeSessions) {
        if (session && !session->isCancelled() &&
            session->getActivityReason() != creatures::runtime::ActivityReason::Idle) {
            return true;
        }
    }

    return false;
}

bool SessionManager::hasActiveNonIdleSessionForCreature(universe_t universe, const creatureId_t &creatureId) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return false;
    }

    for (const auto &session : it->second.activeSessions) {
        if (!session || session->isCancelled()) {
            continue;
        }
        if (session->getActivityReason() == creatures::runtime::ActivityReason::Idle) {
            continue;
        }
        if (sessionHasCreature(session, creatureId)) {
            return true;
        }
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

        // Cancel any playlist sessions
        std::vector<std::shared_ptr<PlaybackSession>> survivors;
        for (auto &session : it->second.activeSessions) {
            if (session && session->getActivityReason() == creatures::runtime::ActivityReason::Playlist &&
                !session->isCancelled()) {
                cancelSessionAndMarkActivity(session);
            } else {
                survivors.push_back(session);
            }
        }
        it->second.activeSessions.swap(survivors);
    }
}

void SessionManager::startPlaylist(universe_t universe, const std::string &playlistId) {
    std::lock_guard<std::mutex> lock(mutex_);

    info("SessionManager: registering playlist start on universe {} (playlist: {})", universe, playlistId);

    auto &state = universeStates_[universe];
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

void SessionManager::clearSession(universe_t universe, const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        debug("SessionManager: clearing session {} for universe {} (preserving playlist state)", sessionId, universe);
        auto &sessions = it->second.activeSessions;
        sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                      [&](const std::shared_ptr<PlaybackSession> &session) {
                                          return session && session->getSessionId() == sessionId;
                                      }),
                       sessions.end());
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

    if (it->second.activeSessions.empty()) {
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
