#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "PlaybackSession.h"
#include "model/Animation.h"
#include "model/PlaylistStatus.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {

/**
 * Represents the current state of a playlist on a universe
 */
enum class PlaylistState {
    None,        // No playlist registered on this universe
    Active,      // Playlist is currently playing normally
    Interrupted, // Playlist is temporarily paused (will resume after interrupt)
    Stopped      // Playlist was explicitly stopped (will not resume)
};

/**
 * SessionManager - Manages active playback sessions and handles interrupts
 *
 * This class provides a central registry for tracking active animation playback,
 * enabling features like:
 * - Interrupting current playback with a new animation
 * - Resuming playlists after interruptions
 * - Querying current playback state
 *
 * Thread-safe for use from multiple threads (event loop, WebSocket handlers, etc.)
 */
class SessionManager {
  public:
    struct PlaylistEventContext {
        uint64_t generation{0};
        std::string triggerTraceId;
        std::string triggerSpanId;
    };
    SessionManager() = default;
    ~SessionManager() = default;

    // Non-copyable, non-movable
    SessionManager(const SessionManager &) = delete;
    SessionManager &operator=(const SessionManager &) = delete;
    SessionManager(SessionManager &&) = delete;
    SessionManager &operator=(SessionManager &&) = delete;

    /**
     * Adopt a new playback session: cancel conflicting sessions and register the new one
     * in a single critical section (issues #62/#63).
     *
     * With the default scope, only sessions overlapping the new session's creatures are
     * cancelled (last request wins per creature). With cancelEntireUniverse=true, every
     * active session on the universe is cancelled first (interrupt semantics).
     *
     * Cancelled sessions get their (cancelled, stopped) activity broadcast inside the
     * critical section — before the caller broadcasts the new session's running state —
     * plus an immediate teardown event. Because cancellation and registration are atomic,
     * the idle-restart check in the playback runner can never observe the gap between
     * "old session cancelled" and "new session registered".
     *
     * Called by CooperativeAnimationScheduler::scheduleAnimation before the new session's
     * activity broadcast and before any audio load; other callers should not need it.
     *
     * @param universe The universe this session is playing on
     * @param session The playback session
     * @param isPlaylist True if this session is part of a playlist
     * @param parentSpan Optional parent for the adoption span
     * @param cancelEntireUniverse Cancel all sessions on the universe, not just overlapping ones
     */
    bool registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist = false,
                         std::shared_ptr<OperationSpan> parentSpan = nullptr, bool cancelEntireUniverse = false,
                         std::optional<uint64_t> expectedPlaylistGeneration = std::nullopt);

    /**
     * Interrupt current playback on a universe with a new animation
     *
     * This will:
     * 1. Cancel any currently playing session on that universe
     * 2. If it was a playlist, save the state for resumption
     * 3. Allow the new animation to play
     *
     * @param universe The universe to interrupt
     * @param interruptAnimation The animation to play as an interrupt
     * @param shouldResumePlaylist Whether to automatically resume playlist after interrupt
     * @param parentSpan Optional parent span
     * @param chainId Stable chain id when this interrupt starts a chained speech
     *                sequence (issue #100) — stamped on the created session so the
     *                whole chain, not just sentence one, owns the queue entries and
     *                the playlist-resume decision
     * @return The session for the interrupt animation, or error
     */
    Result<std::shared_ptr<PlaybackSession>> interrupt(universe_t universe, const Animation &interruptAnimation,
                                                       bool shouldResumePlaylist = false,
                                                       std::shared_ptr<RequestSpan> parentSpan = nullptr,
                                                       const std::string &chainId = {});

    /**
     * Interrupt only idle playback for a specific set of creatures.
     *
     * This is used for ad-hoc animations to avoid preempting active non-idle sessions.
     * Pass every creature the animation targets — a multi-creature dialog interrupts the
     * idle sessions of all of its performers, and conflicts if any of them is busy.
     *
     * @param universe The universe to target
     * @param interruptAnimation The animation to play
     * @param creatureIds The creatures to interrupt idle on
     * @return The session for the ad-hoc animation, or error
     */
    Result<std::shared_ptr<PlaybackSession>> interruptIdleOnly(universe_t universe, const Animation &interruptAnimation,
                                                               const std::vector<creatureId_t> &creatureIds,
                                                               std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Resume playlist playback after an interrupt
     *
     * This will only work if there was a playlist previously interrupted.
     *
     * @param universe The universe to resume on
     * @return True if resume was successful, false if no playlist to resume
     */
    bool resumePlaylist(universe_t universe);

    /**
     * Consume the resume decision for an interrupted playlist.
     *
     * Called by the finishing interrupt session's onFinish. If the interrupt asked
     * for resume, the playlist returns to Active (and its status snapshot is marked
     * playing). If it declined, the playlist transitions to Stopped — the same
     * semantics as stopPlaylist — so onFinish doesn't revive a playlist the client
     * wanted halted. The stored flag used to be write-only and every interrupt
     * auto-resumed (issue #78).
     *
     * @param universe The universe whose interrupted playlist should be decided
     * @return True if the playlist returned to Active and should be rescheduled
     */
    bool consumeInterruptResumeDecision(universe_t universe);

    /**
     * Get the current session on a universe (if any)
     *
     * @param universe The universe to check
     * @return The active session, or nullptr if none
     */
    std::shared_ptr<PlaybackSession> getCurrentSession(universe_t universe) const;

    /**
     * Cancel active sessions on a universe that involve the provided creatures.
     *
     * @param universe The universe to operate on
     * @param creatureIds The creatures to cancel sessions for
     */
    void cancelSessionsForCreatures(universe_t universe, const std::vector<creatureId_t> &creatureIds);

    /**
     * Get a snapshot of active sessions on a universe.
     */
    std::vector<std::shared_ptr<PlaybackSession>> getActiveSessions(universe_t universe) const;

    /**
     * Check if a creature has an active session on the given universe.
     */
    bool hasActiveSessionForCreature(universe_t universe, const creatureId_t &creatureId) const;

    /**
     * Cancel an idle session for a creature on the given universe (if one exists).
     *
     * @return true if an idle session was cancelled
     */
    bool cancelIdleSessionForCreature(universe_t universe, const creatureId_t &creatureId);

    /**
     * Check if a universe is currently playing
     *
     * @param universe The universe to check
     * @return True if there's an active session on this universe
     */
    bool isPlaying(universe_t universe) const;

    /**
     * Check if a universe has any active non-idle sessions.
     *
     * @param universe The universe to check
     * @return True if a non-idle session is active
     */
    bool hasActiveNonIdleSession(universe_t universe) const;

    /**
     * Check if a creature has any active non-idle sessions.
     *
     * @param universe The universe to check
     * @param creatureId The creature to check
     * @return True if a non-idle session is active for that creature
     */
    bool hasActiveNonIdleSessionForCreature(universe_t universe, const creatureId_t &creatureId) const;

    /**
     * Check if a universe has a paused playlist that can be resumed
     *
     * @param universe The universe to check
     * @return True if there's a paused playlist
     */
    bool hasInterruptedPlaylist(universe_t universe) const;

    /**
     * Get the current playlist state for a universe
     *
     * This is the single source of truth for whether a playlist should continue.
     * Use this instead of checking multiple conditions.
     *
     * @param universe The universe to check
     * @return The playlist state (None, Active, Interrupted, or Stopped)
     */
    PlaylistState getPlaylistState(universe_t universe) const;

    /**
     * Mark a playlist as stopped (will not resume)
     *
     * @param universe The universe to stop
     */
    void stopPlaylist(universe_t universe);

    /**
     * Register that a playlist has started on a universe
     *
     * This is called by PlaylistService when a playlist begins.
     * It creates the SessionManager state so getPlaylistState() works correctly.
     *
     * @param universe The universe the playlist is on
     * @param playlistId The ID of the playlist
     */
    PlaylistEventContext startPlaylist(universe_t universe, const std::string &playlistId,
                                       std::string triggerTraceId = {}, std::string triggerSpanId = {});

    /// Claims the one queued PlaylistEvent slot for an active playlist, if available.
    std::optional<PlaylistEventContext> claimPlaylistEvent(universe_t universe,
                                                           std::optional<uint64_t> expectedGeneration = std::nullopt);

    /// Marks a claimed event as executing. A replacement context means a newer start
    /// coalesced into this event while it waited in the event-loop queue.
    std::optional<PlaylistEventContext> beginPlaylistEvent(universe_t universe, uint64_t generation);
    bool isPlaylistGenerationCurrent(universe_t universe, uint64_t generation) const;

    /// Atomically publish an event's status and claim its successor only while
    /// the same playlist generation remains active.
    std::optional<PlaylistEventContext> commitPlaylistEvent(universe_t universe, uint64_t generation,
                                                            const PlaylistStatus &status);

    /**
     * Clear the current session pointer (called when session finishes)
     *
     * This prevents registerSession() from trying to cancel stale sessions.
     * Preserves playlist state (isPlaylist, isInterrupted, etc.)
     *
     * @param universe The universe to clear
     * @param sessionId The session ID to clear
     */
    void clearSession(universe_t universe, const std::string &sessionId);

    struct LoadingSessionAbortResult {
        bool sessionRemoved{false};
        bool playlistCleared{false};
        // The failed session was the interrupter of an Interrupted playlist, so its
        // stored resume decision was applied in-lock: back to Active (resumed) or
        // Stopped. Without this the playlist stayed Interrupted forever when its
        // interrupting session never loaded (issue #100).
        bool playlistResumed{false};
        bool playlistStopped{false};
        std::size_t queuedAnimationsDropped{0};
    };

    /**
     * Atomically remove a session whose asynchronous audio load failed and
     * discard state that only its normal onFinish callback could have drained.
     *
     * If a newer adoption already removed the session, this is a no-op. That
     * ownership check and the queue/playlist cleanup share one mutex section so
     * a late failure cannot erase state belonging to the replacement session.
     *
     * Cleanup is owner-aware (issue #100): only queue entries belonging to the
     * failed session's chain are dropped, playlist state is cleared only when
     * the failed session is the exact current playlist owner, and a failed
     * interrupter resolves the interrupted playlist per its stored decision.
     */
    LoadingSessionAbortResult abortLoadingSession(universe_t universe, const std::string &sessionId);

    /**
     * Queue an animation to play on a universe after the current animation finishes.
     *
     * Used by the streaming ad-hoc speech pipeline to chain sentence animations
     * seamlessly. The queued animation plays automatically when the owning chain's
     * current animation completes — entries are chain-owned (issue #100), so a
     * bystander session finishing on the universe can neither pop nor drop them.
     *
     * The enqueue is refused when no live session carries the chain id: an
     * entry whose chain already died could never be popped. The caller sees
     * that via the return value and can schedule the animation directly
     * instead — a slow TTS render can legitimately outlive its chain's
     * previous sentence.
     *
     * @return true if the animation was queued; false if it was refused
     *         (missing chain id, or no live session carries the chain id)
     */
    [[nodiscard]] bool queueAnimation(universe_t universe, const Animation &animation, const std::string &chainId);

    /**
     * Drop any queued entries owned by the given chain (issue #100).
     *
     * For chain-death paths that don't go through abortLoadingSession or a
     * cancellation — e.g. the onFinish next-hop schedule failing synchronously.
     */
    void dropChainEntries(universe_t universe, const std::string &chainId);

    /**
     * Pop the next queued animation owned by the given chain, if any.
     *
     * Only the front entry is considered, and only when it belongs to the
     * finishing session's chain — a session with no chain id pops nothing.
     *
     * @return The next animation, or std::nullopt
     */
    std::optional<Animation> popQueuedAnimation(universe_t universe, const std::string &chainId);

    /**
     * Check if a universe has queued animations waiting.
     */
    bool hasQueuedAnimation(universe_t universe) const;

    void setPlaylistStatus(universe_t universe, const PlaylistStatus &status);
    std::optional<PlaylistStatus> getPlaylistStatus(universe_t universe) const;
    std::vector<PlaylistStatus> getAllPlaylistStatuses() const;
    void clearPlaylist(universe_t universe);
    bool clearPlaylistIfGeneration(universe_t universe, uint64_t generation);

  private:
    struct UniverseState {
        std::vector<std::shared_ptr<PlaybackSession>> activeSessions;

        // Playlist state machine. One enum instead of the old isPlaylist/isStopped/
        // isInterrupted boolean pile, which could contradict itself — e.g. a playlist
        // stopped via stopPlaylist() (without clearPlaylist()) still had isPlaylist=true,
        // so a later interrupt() marked it interrupted and the onFinish resume logic
        // could revive it (issue #64). All transitions go through the public methods;
        // getPlaylistState() just reads this field.
        PlaylistState playlistState{PlaylistState::None};
        bool shouldResumePlaylist{false}; // Meaningful only while Interrupted

        // Playlist identity + status snapshot for resumption and broadcasts
        std::string playlistId;
        std::optional<PlaylistStatus> playlistStatus;
        PlaylistEventContext playlistEventContext;
        bool playlistEventPending{false};
        uint64_t pendingPlaylistEventGeneration{0};

        // Owner of the playlist state above: the most recently adopted
        // playlist-reason session (issue #100). abortLoadingSession clears the
        // playlist only on an exact owner match, so a stale playlist session's
        // late load failure can't wipe state a newer one owns. Session ids are
        // unique, so the id comparison alone is exact.
        std::string playlistOwnerSessionId;

        // Who owns the resolution of an Interrupted playlist: the interrupting
        // session's chain id (chained speech) or session id (issue #100).
        // Meaningful only while Interrupted. Lets abortLoadingSession apply the
        // stored resume decision when the interrupter dies without ever
        // running, instead of stranding the playlist Interrupted forever.
        std::string interruptOwnerToken;

        // Animation queue for chained playback (streaming ad-hoc speech).
        // Entries are owned by their chain (issue #100).
        struct QueuedAnimation {
            Animation animation;
            std::string chainId;
        };
        std::deque<QueuedAnimation> animationQueue;
    };

    /**
     * Apply an Interrupted playlist's stored resume decision. Caller must hold
     * mutex_ and have verified playlistState == Interrupted. Shared by the
     * normal onFinish path (consumeInterruptResumeDecision) and the
     * failed-interrupter path in abortLoadingSession (issue #100).
     *
     * @return true when the playlist returned to Active (should be rescheduled)
     */
    bool applyInterruptResumeDecisionLocked(universe_t universe, UniverseState &state);

    /** Reset every playlist field of a universe. Caller must hold mutex_. */
    static void clearPlaylistStateLocked(UniverseState &state);

    /**
     * Drop queued entries owned by a chain. Caller must hold mutex_. Used
     * everywhere a chain dies — cancellation during adoption, load-failure
     * aborts, and explicit drops — so dead chains can't strand entries that
     * block the queue (issue #100).
     *
     * @return the number of entries dropped
     */
    static std::size_t eraseChainEntriesLocked(UniverseState &state, const std::string &chainId, universe_t universe);

    mutable std::mutex mutex_;
    std::map<universe_t, UniverseState> universeStates_;
    std::map<universe_t, uint64_t> playlistGenerations_;

    // Monotonic activity-write generation, minted per adoption under mutex_. Because
    // adoptions are serialized here, a later adoption of any creature always carries a
    // higher generation, giving CreatureService a total order for dropping late
    // activity writes (issue #87).
    uint64_t nextActivityGeneration_{1};
};

} // namespace creatures
