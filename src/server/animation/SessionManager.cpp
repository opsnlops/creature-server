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
#include "util/uuidUtils.h"
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

/**
 * Locked phase of session cancellation: flip the flags while the caller holds mutex_.
 * The (cancelled, stopped) broadcast and the teardown scheduling happen after the lock
 * is released via notifyCancelledSession — the broadcast reaches the websocket queue
 * and, on a name-cache miss, the database, neither of which belongs under mutex_
 * (issue #82). markCancellationNotified is set here, under the lock, so any runner
 * that observes the cancellation before our broadcast fires knows not to duplicate it.
 */
void cancelSessionLocked(const std::shared_ptr<PlaybackSession> &session) {
    if (!session) {
        return;
    }
    session->cancel();
    session->markCancellationNotified();
}

void scheduleImmediateTeardown(const std::shared_ptr<PlaybackSession> &session) {
    if (!session || !eventLoop) {
        return;
    }
    auto teardownRunner = std::make_shared<PlaybackRunnerEvent>(eventLoop->getNextFrameNumber(), session);
    eventLoop->scheduleEvent(teardownRunner);
}

/**
 * Post-lock phase of session cancellation: activity broadcast, idle counters, and the
 * immediate teardown runner. Must be called on the same thread that ran the locked
 * phase, before any (reason, running) broadcast for a replacement session, so the
 * (cancelled, stopped) → (reason, running) ordering the fixture binding dispatcher
 * depends on still holds.
 */
void notifyCancelledSession(const std::shared_ptr<PlaybackSession> &session) {
    if (!session) {
        return;
    }
    if (session->getActivityReason() == creatures::runtime::ActivityReason::Idle) {
        creatures::ws::CreatureService::incrementIdleStopped(session->getCreatureIds());
    }
    creatures::ws::CreatureService::setActivityState(
        session->getCreatureIds(), session->getAnimation().id, creatures::runtime::ActivityReason::Cancelled,
        creatures::runtime::ActivityState::Stopped, session->getSessionId(), nullptr, session->getActivityGeneration());
    scheduleImmediateTeardown(session);
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

    std::vector<std::shared_ptr<PlaybackSession>> cancelledSessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Mint the activity-write generation before the session is published. Adoptions
        // are serialized by mutex_, so a later adoption always carries a higher
        // generation — the total order CreatureService uses to drop late activity
        // writes, including late Running writes (issue #87).
        session->setActivityGeneration(nextActivityGeneration_++);

        // Cancel conflicting sessions and register the new one under the same lock — this
        // atomicity is what keeps the idle-restart check from ever seeing the universe as
        // free between the cancel and the registration (issue #62). Only the flag flips
        // happen here; the (cancelled, stopped) broadcasts run after the lock is released
        // (issue #82) but still on this thread, before the caller's (reason, running)
        // broadcast — the ordering the fixture binding dispatcher depends on.
        auto it = universeStates_.find(universe);
        if (it != universeStates_.end()) {
            std::vector<std::shared_ptr<PlaybackSession>> survivors;
            survivors.reserve(it->second.activeSessions.size());
            for (auto &existing : it->second.activeSessions) {
                if (!existing) {
                    continue;
                }
                if (!existing->isCancelled() && (cancelEntireUniverse || sessionsConflict(session, existing))) {
                    debug("SessionManager: cancelling {} session on universe {} for new session",
                          cancelEntireUniverse ? "active" : "overlapping", universe);
                    cancelSessionLocked(existing);
                    cancelledSessions.push_back(existing);
                    // A cancelled chain is dead — its onFinish early-returns
                    // without popping, so drop its queued sentences now. Not
                    // when the replacement continues the same chain (issue #100).
                    if (!existing->getChainId().empty() && existing->getChainId() != session->getChainId()) {
                        eraseChainEntriesLocked(it->second, existing->getChainId(), universe);
                    }
                } else {
                    survivors.push_back(existing);
                }
            }
            it->second.activeSessions.swap(survivors);
        }

        // Register new session - preserve existing playlist state if present
        if (it != universeStates_.end()) {
            // Preserve existing playlist state (playlistState, playlistId, etc.)
            it->second.activeSessions.push_back(session);
            // Only promote to Active when explicitly registering a playlist session
            if (isPlaylist) {
                it->second.playlistState = PlaylistState::Active;
                it->second.playlistOwnerSessionId = session->getSessionId();
            }
            debug("SessionManager: updated session on universe {} (playlist_state: {}, active_sessions: {})", universe,
                  static_cast<int>(it->second.playlistState), it->second.activeSessions.size());
        } else {
            // No existing state, create new
            UniverseState state;
            state.activeSessions.push_back(session);
            state.playlistState = isPlaylist ? PlaylistState::Active : PlaylistState::None;
            if (isPlaylist) {
                state.playlistOwnerSessionId = session->getSessionId();
            }
            universeStates_[universe] = state;
            info("SessionManager: registered new session on universe {} (playlist: {})", universe, isPlaylist);
        }
    }

    for (const auto &cancelledSession : cancelledSessions) {
        notifyCancelledSession(cancelledSession);
    }

    if (span) {
        if (!cancelledSessions.empty()) {
            span->setAttribute("adopt.cancelled_sessions", static_cast<int64_t>(cancelledSessions.size()));
        }
        span->setAttribute("session.id", session->getSessionId());
        span->setSuccess();
    }
}

Result<std::shared_ptr<PlaybackSession>>
SessionManager::interrupt(universe_t universe, const Animation &interruptAnimation, bool shouldResumePlaylist,
                          std::shared_ptr<RequestSpan> parentSpan, const std::string &chainId) {
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
    bool tookOverInterrupted = false;
    bool previousShouldResume = false;
    std::string previousOwnerToken;

    // Every interrupt owns the resolution of the playlist it pauses. For chained
    // speech the token is the stable chain id; otherwise mint a one-off token that
    // is also stamped on the session (as its chain id) by scheduleAnimation. The
    // token must exist BEFORE the schedule call: scheduleAnimation submits the
    // async audio load, and a fast load failure runs abortLoadingSession
    // immediately — stamping afterwards would let that abort miss the stored
    // resume decision and strand the playlist Interrupted (issue #100 review).
    const std::string ownerToken = chainId.empty() ? creatures::util::generateUUID() : chainId;

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
                it->second.interruptOwnerToken = ownerToken;
                interruptedPlaylist = true;
                info("SessionManager: marked playlist on universe {} as interrupted (resume: {})", universe,
                     shouldResumePlaylist);
            } else if (it->second.playlistState == PlaylistState::Interrupted) {
                // A second interrupt over a still-pending one takes over the whole
                // resolution — token AND decision — matching the onFinish rule that
                // the replacing interrupt handles resume when IT finishes. Taking
                // only the token would resolve with the previous interrupt's
                // decision (issue #100 review).
                previousShouldResume = it->second.shouldResumePlaylist;
                previousOwnerToken = it->second.interruptOwnerToken;
                it->second.shouldResumePlaylist = shouldResumePlaylist;
                it->second.interruptOwnerToken = ownerToken;
                tookOverInterrupted = true;
                info("SessionManager: interrupt takeover on universe {} (resume: {})", universe, shouldResumePlaylist);
            }
        }
    }

    if (span) {
        span->setAttribute("interrupted_playlist", interruptedPlaylist);
        span->setAttribute("interrupt.took_over_pending", tookOverInterrupted);
    }

    // Schedule the interrupt animation. Adoption inside scheduleAnimation cancels every
    // active session on the universe and registers the new one in one critical section.
    auto sessionResult = CooperativeAnimationScheduler::scheduleAnimation(
        eventLoop->getNextFrameNumber(), interruptAnimation, universe, creatures::runtime::ActivityReason::AdHoc,
        /*cancelEntireUniverse=*/true, ownerToken);

    if (!sessionResult.isSuccess()) {
        // Undo the ownership this interrupt claimed. Guarded on the state still
        // being Interrupted with OUR token: a post-adoption commit failure has
        // already run abortLoadingSession, which resolved the playlist per the
        // stored decision — that resolution must stand.
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = universeStates_.find(universe);
            if (it != universeStates_.end() && it->second.playlistState == PlaylistState::Interrupted &&
                it->second.interruptOwnerToken == ownerToken) {
                if (interruptedPlaylist) {
                    // Nothing was cancelled — the playlist keeps playing.
                    it->second.playlistState = PlaylistState::Active;
                    it->second.shouldResumePlaylist = false;
                    it->second.interruptOwnerToken.clear();
                } else if (tookOverInterrupted) {
                    // Hand the resolution back to the interrupt we tried to replace.
                    it->second.shouldResumePlaylist = previousShouldResume;
                    it->second.interruptOwnerToken = previousOwnerToken;
                }
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
    it->second.interruptOwnerToken.clear();
    if (it->second.playlistStatus) {
        it->second.playlistStatus->playing = true;
    }

    return true;
}

bool SessionManager::consumeInterruptResumeDecision(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.playlistState != PlaylistState::Interrupted) {
        return false;
    }

    return applyInterruptResumeDecisionLocked(universe, it->second);
}

bool SessionManager::applyInterruptResumeDecisionLocked(universe_t universe, UniverseState &state) {
    state.interruptOwnerToken.clear();

    if (state.shouldResumePlaylist) {
        info("SessionManager: resuming playlist on universe {} after interrupt", universe);
        state.playlistState = PlaylistState::Active;
        state.shouldResumePlaylist = false;
        if (state.playlistStatus) {
            state.playlistStatus->playing = true;
        }
        return true;
    }

    info("SessionManager: interrupt on universe {} declined playlist resume; stopping playlist", universe);
    state.playlistState = PlaylistState::Stopped;
    if (state.playlistStatus) {
        state.playlistStatus->playing = false;
        state.playlistStatus->current_animation.clear();
    }
    return false;
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
    std::vector<std::shared_ptr<PlaybackSession>> cancelledSessions;

    {
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
                cancelSessionLocked(session);
                cancelledSessions.push_back(session);
                eraseChainEntriesLocked(it->second, session->getChainId(), universe);
            } else {
                survivors.push_back(session);
            }
        }
        it->second.activeSessions.swap(survivors);
    }

    // notifyCancelledSession also schedules the immediate teardown runner — this path
    // (streaming takeover) used to skip it, leaving the cancelled session's audio
    // playing until its next scheduled runner fired (issue #84).
    for (const auto &cancelledSession : cancelledSessions) {
        notifyCancelledSession(cancelledSession);
    }
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
    std::vector<std::shared_ptr<PlaybackSession>> cancelledSessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = universeStates_.find(universe);
        if (it == universeStates_.end()) {
            return false;
        }

        std::vector<std::shared_ptr<PlaybackSession>> survivors;
        survivors.reserve(it->second.activeSessions.size());
        for (auto &session : it->second.activeSessions) {
            if (session && !session->isCancelled() &&
                session->getActivityReason() == creatures::runtime::ActivityReason::Idle &&
                sessionHasCreature(session, creatureId)) {
                debug("SessionManager: cancelling idle session on universe {} for creature {}", universe, creatureId);
                cancelSessionLocked(session);
                cancelledSessions.push_back(session);
            } else {
                survivors.push_back(session);
            }
        }
        it->second.activeSessions.swap(survivors);
    }

    for (const auto &cancelledSession : cancelledSessions) {
        notifyCancelledSession(cancelledSession);
    }
    return !cancelledSessions.empty();
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
    std::vector<std::shared_ptr<PlaybackSession>> cancelledSessions;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++playlistGenerations_[universe];

        auto it = universeStates_.find(universe);
        if (it == universeStates_.end() || (it->second.playlistState != PlaylistState::Active &&
                                            it->second.playlistState != PlaylistState::Interrupted)) {
            return;
        }

        it->second.playlistEventPending = false;
        it->second.pendingPlaylistEventGeneration = 0;

        info("SessionManager: stopping playlist on universe {}", universe);
        it->second.playlistState = PlaylistState::Stopped;
        it->second.shouldResumePlaylist = false;
        it->second.interruptOwnerToken.clear();
        if (it->second.playlistStatus) {
            it->second.playlistStatus->playing = false;
            it->second.playlistStatus->current_animation.clear();
        }

        // Cancel any playlist sessions
        std::vector<std::shared_ptr<PlaybackSession>> survivors;
        for (auto &session : it->second.activeSessions) {
            if (session && session->getActivityReason() == creatures::runtime::ActivityReason::Playlist &&
                !session->isCancelled()) {
                cancelSessionLocked(session);
                cancelledSessions.push_back(session);
                eraseChainEntriesLocked(it->second, session->getChainId(), universe);
            } else {
                survivors.push_back(session);
            }
        }
        it->second.activeSessions.swap(survivors);
    }

    for (const auto &cancelledSession : cancelledSessions) {
        notifyCancelledSession(cancelledSession);
    }
}

SessionManager::PlaylistEventContext SessionManager::startPlaylist(universe_t universe, const std::string &playlistId,
                                                                   std::string triggerTraceId,
                                                                   std::string triggerSpanId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t generation = ++playlistGenerations_[universe];

    info("SessionManager: registering playlist start on universe {} (playlist: {})", universe, playlistId);

    auto &state = universeStates_[universe];
    state.playlistState = PlaylistState::Active;
    state.shouldResumePlaylist = false;
    state.interruptOwnerToken.clear();
    // No session has been adopted for this playlist yet; the first
    // registerSession(isPlaylist=true) stamps the owner (issue #100).
    state.playlistOwnerSessionId.clear();
    state.playlistId = playlistId;
    if (!state.playlistStatus) {
        state.playlistStatus = PlaylistStatus{};
        state.playlistStatus->universe = universe;
    }
    state.playlistStatus->playlist = playlistId;
    state.playlistStatus->playing = true;
    state.playlistEventContext = {generation, std::move(triggerTraceId), std::move(triggerSpanId)};
    return state.playlistEventContext;
}

std::optional<SessionManager::PlaylistEventContext> SessionManager::claimPlaylistEvent(universe_t universe) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.playlistState != PlaylistState::Active ||
        it->second.playlistEventContext.generation == 0 || it->second.playlistEventPending) {
        return std::nullopt;
    }
    it->second.playlistEventPending = true;
    it->second.pendingPlaylistEventGeneration = it->second.playlistEventContext.generation;
    return it->second.playlistEventContext;
}

std::optional<SessionManager::PlaylistEventContext> SessionManager::beginPlaylistEvent(universe_t universe,
                                                                                       uint64_t generation) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.playlistState != PlaylistState::Active ||
        it->second.playlistEventContext.generation == 0) {
        if (it != universeStates_.end()) {
            it->second.playlistEventPending = false;
            it->second.pendingPlaylistEventGeneration = 0;
        }
        return std::nullopt;
    }
    if (it->second.playlistEventContext.generation != generation) {
        // A newer event is already queued for the current generation (for
        // example after clear + start), so this old event has nothing to hand off.
        if (it->second.pendingPlaylistEventGeneration != generation) {
            return std::nullopt;
        }
        it->second.pendingPlaylistEventGeneration = it->second.playlistEventContext.generation;
        return it->second.playlistEventContext;
    }
    it->second.playlistEventPending = false;
    it->second.pendingPlaylistEventGeneration = 0;
    return it->second.playlistEventContext;
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

SessionManager::LoadingSessionAbortResult SessionManager::abortLoadingSession(universe_t universe,
                                                                              const std::string &sessionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    LoadingSessionAbortResult result;

    auto stateIt = universeStates_.find(universe);
    if (stateIt == universeStates_.end()) {
        return result;
    }

    auto &state = stateIt->second;
    auto sessionIt = std::find_if(state.activeSessions.begin(), state.activeSessions.end(),
                                  [&](const std::shared_ptr<PlaybackSession> &candidate) {
                                      return candidate && candidate->getSessionId() == sessionId;
                                  });
    if (sessionIt == state.activeSessions.end()) {
        return result;
    }

    // Copy identity before erasing the iterator; the shared_ptr keeps the
    // session alive but the vector slot doesn't.
    const auto abortedSession = *sessionIt;
    const bool wasPlaylist = abortedSession->getActivityReason() == creatures::runtime::ActivityReason::Playlist;
    const std::string chainId = abortedSession->getChainId();
    const std::string ownerToken = chainId.empty() ? sessionId : chainId;
    state.activeSessions.erase(sessionIt);
    result.sessionRemoved = true;

    // Drop only the failed session's own chain entries. A bystander chain's
    // queued sentences — or any other session's state — must survive this
    // session's failure (issue #100).
    if (!chainId.empty()) {
        result.queuedAnimationsDropped =
            std::erase_if(state.animationQueue, [&chainId](const UniverseState::QueuedAnimation &queued) {
                return queued.chainId == chainId;
            });
    }

    // A failed interrupter resolves the playlist it interrupted per its stored
    // decision — otherwise the playlist would stay Interrupted forever, since
    // the session that was supposed to consume the decision on finish never
    // ran (issue #100). The owner token covers the whole chain, so a failing
    // sentence-two still resolves what sentence-one's interrupt started.
    if (state.playlistState == PlaylistState::Interrupted && !state.interruptOwnerToken.empty() &&
        state.interruptOwnerToken == ownerToken) {
        const bool resumed = applyInterruptResumeDecisionLocked(universe, state);
        result.playlistResumed = resumed;
        result.playlistStopped = !resumed;
    }

    // Clear playlist state only when the failed session is its exact current
    // owner; another playlist session's state survives (issue #100).
    if (wasPlaylist && state.playlistOwnerSessionId == sessionId) {
        clearPlaylistStateLocked(state);
        result.playlistCleared = true;
    }

    if (state.activeSessions.empty() && state.playlistState == PlaylistState::None && state.animationQueue.empty()) {
        universeStates_.erase(stateIt);
    }

    return result;
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
    clearPlaylistStateLocked(it->second);

    // Queued chain entries are load-bearing state: a live speech chain may be
    // between hops (its next session not yet registered) when a stop-playlist
    // request lands, and erasing the whole universe state here would silently
    // truncate it (issue #100).
    if (it->second.activeSessions.empty() && it->second.animationQueue.empty()) {
        universeStates_.erase(it);
    }
}

void SessionManager::clearPlaylistStateLocked(UniverseState &state) {
    state.playlistState = PlaylistState::None;
    state.shouldResumePlaylist = false;
    state.playlistId.clear();
    state.playlistStatus.reset();
    state.playlistOwnerSessionId.clear();
    state.interruptOwnerToken.clear();
}

std::size_t SessionManager::eraseChainEntriesLocked(UniverseState &state, const std::string &chainId,
                                                    universe_t universe) {
    if (chainId.empty() || state.animationQueue.empty()) {
        return 0;
    }
    const auto dropped = std::erase_if(state.animationQueue, [&chainId](const UniverseState::QueuedAnimation &queued) {
        return queued.chainId == chainId;
    });
    if (dropped > 0) {
        warn("SessionManager: dropped {} queued animation(s) for dead chain {} on universe {}", dropped, chainId,
             universe);
    }
    return dropped;
}

// --- Animation Queue ---

bool SessionManager::queueAnimation(universe_t universe, const Animation &animation, const std::string &chainId) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (chainId.empty()) {
        warn("SessionManager: refusing to queue animation '{}' on universe {} with no chain id — an ownerless entry "
             "could never be popped (issue #100)",
             animation.metadata.title, universe);
        return false;
    }

    // Only accept entries while a live session of this chain exists: with
    // chain-owned popping, an entry enqueued after its chain died could never
    // be drained and would sit in the universe state forever (issue #100).
    // Cancelled sessions don't count — their onFinish skips the queue.
    auto it = universeStates_.find(universe);
    bool chainAlive = false;
    if (it != universeStates_.end()) {
        for (const auto &session : it->second.activeSessions) {
            if (session && !session->isCancelled() && session->getChainId() == chainId) {
                chainAlive = true;
                break;
            }
        }
    }
    if (!chainAlive) {
        warn("SessionManager: not queueing animation '{}' on universe {} — its chain {} has no live session",
             animation.metadata.title, universe, chainId);
        return false;
    }

    it->second.animationQueue.push_back(UniverseState::QueuedAnimation{animation, chainId});
    info("SessionManager: queued animation '{}' on universe {} for chain {} (queue depth: {})",
         animation.metadata.title, universe, chainId, it->second.animationQueue.size());
    return true;
}

void SessionManager::dropChainEntries(universe_t universe, const std::string &chainId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end()) {
        return;
    }
    eraseChainEntriesLocked(it->second, chainId, universe);
}

std::optional<Animation> SessionManager::popQueuedAnimation(universe_t universe, const std::string &chainId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    if (it == universeStates_.end() || it->second.animationQueue.empty()) {
        return std::nullopt;
    }
    // Only the owning chain may drain its entries — a bystander session
    // finishing on the same universe must not start another chain's sentence.
    // Match the finisher's EARLIEST entry rather than only the front, so a
    // foreign entry (another live chain's, or a not-yet-purged orphan) can't
    // block this chain (issue #100 review).
    if (chainId.empty()) {
        return std::nullopt;
    }
    auto &queue = it->second.animationQueue;
    auto entryIt = std::find_if(queue.begin(), queue.end(), [&chainId](const UniverseState::QueuedAnimation &queued) {
        return queued.chainId == chainId;
    });
    if (entryIt == queue.end()) {
        return std::nullopt;
    }
    auto animation = std::move(entryIt->animation);
    queue.erase(entryIt);
    debug("SessionManager: popped queued animation '{}' from universe {} for chain {} (remaining: {})",
          animation.metadata.title, universe, chainId, it->second.animationQueue.size());
    return animation;
}

bool SessionManager::hasQueuedAnimation(universe_t universe) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = universeStates_.find(universe);
    return it != universeStates_.end() && !it->second.animationQueue.empty();
}

} // namespace creatures
