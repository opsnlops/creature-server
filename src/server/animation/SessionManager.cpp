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
#include "util/helpers.h"
#include <algorithm>
#include <unordered_set>

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;

namespace {

bool sessionHasCreature(const std::shared_ptr<PlaybackSession> &session, const creatureId_t &creatureId) {
    if (!session || creatureId.empty()) {
        return false;
    }
    const auto &ids = session->getCreatureIds();
    return std::find(ids.begin(), ids.end(), creatureId) != ids.end();
}

bool overlaps(const std::unordered_set<creatureId_t> &lhs, const std::shared_ptr<PlaybackSession> &session) {
    if (!session) {
        return false;
    }
    for (const auto &id : session->getCreatureIds()) {
        if (lhs.count(id) > 0) {
            return true;
        }
    }
    return false;
}

/**
 * Do two sessions contend for the same output? True when they share a creature or a
 * fixture. The old creature-only check also treated the *empty* id as shared: any two
 * sessions that each contained a fixture track "overlapped" through creatureId == ""
 * and cancelled each other regardless of which fixture they drove (issue #65).
 */
bool sessionsConflict(const std::shared_ptr<PlaybackSession> &a, const std::shared_ptr<PlaybackSession> &b) {
    if (!a || !b) {
        return false;
    }
    const auto &aCreatures = a->getCreatureIds();
    for (const auto &id : b->getCreatureIds()) {
        if (std::find(aCreatures.begin(), aCreatures.end(), id) != aCreatures.end()) {
            return true;
        }
    }
    const auto &aFixtures = a->getFixtureIds();
    for (const auto &id : b->getFixtureIds()) {
        if (std::find(aFixtures.begin(), aFixtures.end(), id) != aFixtures.end()) {
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
        creatures::ws::CreatureService::incrementIdleStopped(session->getCreatureIds());
    }

    session->cancel();
    session->markCancellationNotified();
    creatures::ws::CreatureService::setActivityState(
        session->getCreatureIds(), session->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
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

void SessionManager::registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist,
                                     std::shared_ptr<OperationSpan> parentSpan, bool cancelEntireUniverse) {
    if (!session) {
        warn("SessionManager: attempted to register null session on universe {}", universe);
        return;
    }

    auto span =
        observability ? observability->createChildOperationSpan("SessionManager.registerSession", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("is_playlist", isPlaylist);
        span->setAttribute("adopt.cancel_entire_universe", cancelEntireUniverse);
        span->setAttribute("session.id", session->getSessionId());
        span->setAttribute("session.animation_id", session->getAnimation().id);
    }

    std::lock_guard<std::mutex> lock(mutex_);

    // Cancel conflicting sessions and register the new one under the same lock — this
    // atomicity is what keeps the idle-restart check from ever seeing the universe as
    // free between the cancel and the registration (issue #62). The (cancelled, stopped)
    // broadcasts fired here land before the caller's (reason, running) broadcast, which
    // is the ordering the fixture binding dispatcher depends on.
    auto it = universeStates_.find(universe);
    if (it != universeStates_.end()) {
        std::vector<std::shared_ptr<PlaybackSession>> survivors;
        size_t cancelled = 0;
        survivors.reserve(it->second.activeSessions.size());
        for (auto &existing : it->second.activeSessions) {
            if (!existing) {
                continue;
            }
            if (!existing->isCancelled() && (cancelEntireUniverse || sessionsConflict(session, existing))) {
                debug("SessionManager: cancelling {} session on universe {} for new session",
                      cancelEntireUniverse ? "active" : "overlapping", universe);
                cancelSessionAndMarkActivity(existing);
                scheduleImmediateTeardown(existing);
                cancelled++;
            } else {
                survivors.push_back(existing);
            }
        }
        it->second.activeSessions.swap(survivors);
        if (span && cancelled > 0) {
            span->setAttribute("adopt.cancelled_sessions", static_cast<int64_t>(cancelled));
        }
    }

    // Register new session - preserve existing playlist state if present
    if (it != universeStates_.end()) {
        // Preserve existing playlist state (playlistState, playlistId, etc.)
        it->second.activeSessions.push_back(session);
        // Only promote to Active when explicitly registering a playlist session
        if (isPlaylist) {
            it->second.playlistState = PlaylistState::Active;
        }
        debug("SessionManager: updated session on universe {} (playlist_state: {}, active_sessions: {})", universe,
              static_cast<int>(it->second.playlistState), it->second.activeSessions.size());
    } else {
        // No existing state, create new
        UniverseState state;
        state.activeSessions.push_back(session);
        state.playlistState = isPlaylist ? PlaylistState::Active : PlaylistState::None;
        universeStates_[universe] = state;
        info("SessionManager: registered new session on universe {} (playlist: {})", universe, isPlaylist);
    }

    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
}

Result<std::shared_ptr<PlaybackSession>> SessionManager::interrupt(universe_t universe,
                                                                   const Animation &interruptAnimation,
                                                                   bool shouldResumePlaylist,
                                                                   std::shared_ptr<RequestSpan> parentSpan) {
    auto span = observability ? observability->createOperationSpan("SessionManager.interrupt", parentSpan) : nullptr;
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

    // Mark the playlist interrupted *before* scheduling so PlaylistEvents pause and the
    // onFinish resume logic knows to restart it. The sessions themselves are NOT cancelled
    // here: that happens inside registerSession's adoption (cancelEntireUniverse=true),
    // atomically with the new session's registration, from within scheduleAnimation.
    // This closes the idle-restart race the old code prevented by holding mutex_ across
    // the whole schedule — which stalled the event loop for the duration of the audio
    // load whenever a cancelled session's teardown touched the mutex (issues #62/#63).
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end()) {
            if (!it->second.activeSessions.empty()) {
                info("SessionManager: interrupting playback on universe {} with animation '{}'", universe,
                     interruptAnimation.metadata.title);
            }
            // Only an *active* playlist can be interrupted. The old boolean version also
            // marked stopped playlists interrupted (isPlaylist stayed true after
            // stopPlaylist), letting the onFinish resume logic revive them (issue #64).
            if (it->second.playlistState == PlaylistState::Active) {
                it->second.playlistState = PlaylistState::Interrupted;
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

    // Schedule the interrupt animation. Adoption inside scheduleAnimation cancels every
    // active session on the universe and registers the new one in one critical section.
    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(
        eventLoop->getNextFrameNumber(), interruptAnimation, universe, creatures::runtime::ActivityReason::AdHoc,
        /*cancelEntireUniverse=*/true);

    if (!sessionResult.isSuccess()) {
        // Nothing was cancelled — undo the playlist-interrupted mark so it keeps playing.
        if (interruptedPlaylist) {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = universeStates_.find(universe);
            if (it != universeStates_.end() && it->second.playlistState == PlaylistState::Interrupted) {
                it->second.playlistState = PlaylistState::Active;
                it->second.shouldResumePlaylist = false;
            }
        }
        error("SessionManager: failed to schedule interrupt animation: {}", sessionResult.getError()->getMessage());
        if (span) {
            span->setError(sessionResult.getError()->getMessage());
        }
        return sessionResult;
    }

    auto session = sessionResult.getValue().value();

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
                                                                           const std::vector<creatureId_t> &creatureIds,
                                                                           std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("SessionManager.interruptIdleOnly", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("universe", static_cast<int64_t>(universe));
        span->setAttribute("interrupt.animation_id", interruptAnimation.id);
        span->setAttribute("interrupt.animation_title", interruptAnimation.metadata.title);
        span->setAttribute("creature.ids", joinStrings(creatureIds, ","));
    }

    if (creatureIds.empty()) {
        std::string errorMessage = "SessionManager: interruptIdleOnly called with no creatures";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InvalidData, errorMessage)};
    }

    if (!eventLoop) {
        std::string errorMessage = "SessionManager: event loop unavailable";
        error(errorMessage);
        if (span) {
            span->setError(errorMessage);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::InternalError, errorMessage)};
    }

    const std::unordered_set<creatureId_t> targets(creatureIds.begin(), creatureIds.end());

    // Every target creature that's mid-performance, deduped. Collected in full (not
    // first-hit) so the error can name all of the busy performers, and reported outside
    // the lock because name resolution can hit the database.
    //
    // Only the busy *check* happens here. Cancelling the targets' idle sessions is left
    // to registerSession's adoption inside scheduleAnimation, where it happens atomically
    // with the new session's registration (issue #62). The old code cancelled here and
    // registered later, reopening the idle-restart race that interrupt() documents.
    std::vector<creatureId_t> busyCreatures;

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
                for (const auto &creatureId : existing->getCreatureIds()) {
                    if (targets.count(creatureId) > 0 &&
                        std::find(busyCreatures.begin(), busyCreatures.end(), creatureId) == busyCreatures.end()) {
                        busyCreatures.push_back(creatureId);
                    }
                }
            }
        }
    }

    if (!busyCreatures.empty()) {
        std::vector<std::string> busyNames;
        busyNames.reserve(busyCreatures.size());
        for (const auto &busyId : busyCreatures) {
            busyNames.push_back(creatures::ws::CreatureService::resolveCreatureName(busyId));
        }
        std::string who = busyNames.back();
        if (busyNames.size() > 1) {
            const std::vector<std::string> allButLast(busyNames.begin(), busyNames.end() - 1);
            who = joinStrings(allButLast, ", ") + " and " + busyNames.back();
        }
        const std::string errorMessage = who + (busyNames.size() == 1 ? " already has an active non-idle session"
                                                                      : " already have active non-idle sessions");
        if (span) {
            span->setAttribute("conflict.creature_ids", joinStrings(busyCreatures, ","));
            span->setError(errorMessage);
        }
        return Result<std::shared_ptr<PlaybackSession>>{ServerError(ServerError::Conflict, errorMessage)};
    }

    // Adoption inside scheduleAnimation cancels the targets' idle sessions (any session
    // overlapping the animation's creatures) and registers the new one atomically. A
    // non-idle session that slipped in since the busy check above gets cancelled too —
    // same last-request-wins semantics as every other registration path.
    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(
        eventLoop->getNextFrameNumber(), interruptAnimation, universe, creatures::runtime::ActivityReason::AdHoc);

    if (!sessionResult.isSuccess()) {
        if (span) {
            span->setError(sessionResult.getError()->getMessage());
        }
        return sessionResult;
    }

    auto session = sessionResult.getValue().value();

    if (span) {
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }

    return Result<std::shared_ptr<PlaybackSession>>{session};
}

bool SessionManager::resumePlaylist(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.playlistState != PlaylistState::Interrupted) {
        debug("SessionManager: no interrupted playlist to resume on universe {}", universe);
        return false;
    }

    // Back to Active so PlaylistEvents can schedule animations again
    info("SessionManager: resuming playlist on universe {}", universe);
    it->second.playlistState = PlaylistState::Active;
    it->second.shouldResumePlaylist = false;
    if (it->second.playlistStatus) {
        it->second.playlistStatus->playing = true;
    }

    return true;
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
    return (it != universeStates_.end() && it->second.playlistState == PlaylistState::Interrupted);
}

PlaylistState SessionManager::getPlaylistState(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return PlaylistState::None;
    }

    return it->second.playlistState;
}

void SessionManager::stopPlaylist(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it != universeStates_.end() &&
        (it->second.playlistState == PlaylistState::Active || it->second.playlistState == PlaylistState::Interrupted)) {
        info("SessionManager: stopping playlist on universe {}", universe);
        it->second.playlistState = PlaylistState::Stopped;
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
    state.playlistState = PlaylistState::Active;
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

    // Snapshot-only by design: playlist lifecycle transitions go through startPlaylist/
    // stopPlaylist/resumePlaylist/clearPlaylist. The old code derived isStopped from
    // status.playing here, which is how contradictory states were born (issue #64). The
    // one defensive promotion kept: a playing snapshot for a universe with no playlist
    // state means the caller skipped startPlaylist — treat it as Active.
    if (state.playlistState == PlaylistState::None && !status.playlist.empty() && status.playing) {
        state.playlistState = PlaylistState::Active;
    }
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
    it->second.playlistState = PlaylistState::None;
    it->second.shouldResumePlaylist = false;
    it->second.playlistId.clear();
    it->second.playlistStatus.reset();

    if (it->second.activeSessions.empty()) {
        universeStates_.erase(it);
    }
}

// --- Animation Queue ---

void SessionManager::queueAnimation(universe_t universe, const Animation &animation) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &state = universeStates_[universe];
    state.animationQueue.push(animation);
    info("SessionManager: queued animation '{}' on universe {} (queue depth: {})", animation.metadata.title, universe,
         state.animationQueue.size());
}

std::optional<Animation> SessionManager::popQueuedAnimation(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.animationQueue.empty()) {
        return std::nullopt;
    }
    auto animation = std::move(it->second.animationQueue.front());
    it->second.animationQueue.pop();
    debug("SessionManager: popped queued animation '{}' from universe {} (remaining: {})", animation.metadata.title,
          universe, it->second.animationQueue.size());
    return animation;
}

bool SessionManager::hasQueuedAnimation(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    return it != universeStates_.end() && !it->second.animationQueue.empty();
}

} // namespace creatures
