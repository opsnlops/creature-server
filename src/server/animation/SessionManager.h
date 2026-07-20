#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
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
    void registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist = false,
                         std::shared_ptr<OperationSpan> parentSpan = nullptr, bool cancelEntireUniverse = false);

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
     * @return The session for the interrupt animation, or error
     */
    Result<std::shared_ptr<PlaybackSession>> interrupt(universe_t universe, const Animation &interruptAnimation,
                                                       bool shouldResumePlaylist = false,
                                                       std::shared_ptr<RequestSpan> parentSpan = nullptr);

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
    void startPlaylist(universe_t universe, const std::string &playlistId);

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

    /**
     * Queue an animation to play on a universe after the current animation finishes.
     *
     * Used by the streaming ad-hoc speech pipeline to chain sentence animations
     * seamlessly. The queued animation plays automatically when the current one
     * completes — no callback chaining needed.
     */
    void queueAnimation(universe_t universe, const Animation &animation);

    /**
     * Pop the next queued animation for a universe, if any.
     *
     * @return The next animation, or std::nullopt if the queue is empty
     */
    std::optional<Animation> popQueuedAnimation(universe_t universe);

    /**
     * Check if a universe has queued animations waiting.
     */
    bool hasQueuedAnimation(universe_t universe) const;

    void setPlaylistStatus(universe_t universe, const PlaylistStatus &status);
    std::optional<PlaylistStatus> getPlaylistStatus(universe_t universe) const;
    std::vector<PlaylistStatus> getAllPlaylistStatuses() const;
    void clearPlaylist(universe_t universe);

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

        // Animation queue for chained playback (streaming ad-hoc speech)
        std::queue<Animation> animationQueue;
    };

    mutable std::mutex mutex_;
    std::map<universe_t, UniverseState> universeStates_;
};

} // namespace creatures
