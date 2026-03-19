#pragma once

#include <atomic>
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
    SessionManager() = default;
    ~SessionManager() = default;

    // Non-copyable, non-movable
    SessionManager(const SessionManager &) = delete;
    SessionManager &operator=(const SessionManager &) = delete;
    SessionManager(SessionManager &&) = delete;
    SessionManager &operator=(SessionManager &&) = delete;

    /**
     * Register a new playback session
     *
     * Cancels only overlapping creature sessions on the same universe (last request wins per creature).
     *
     * @param universe The universe this session is playing on
     * @param session The playback session
     * @param isPlaylist True if this session is part of a playlist
     */
    void registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist = false,
                         std::shared_ptr<RequestSpan> parentSpan = nullptr);

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
     * Interrupt only idle playback for a specific creature.
     *
     * This is used for ad-hoc animations to avoid preempting active non-idle sessions.
     *
     * @param universe The universe to target
     * @param interruptAnimation The animation to play
     * @param creatureId The creature to interrupt idle on
     * @return The session for the ad-hoc animation, or error
     */
    Result<std::shared_ptr<PlaybackSession>> interruptIdleOnly(universe_t universe, const Animation &interruptAnimation,
                                                               const creatureId_t &creatureId,
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
     * Cancel all playback on a universe
     *
     * @param universe The universe to cancel
     */
    void cancelUniverse(universe_t universe);

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

    void setPlaylistStatus(universe_t universe, const PlaylistStatus &status);
    std::optional<PlaylistStatus> getPlaylistStatus(universe_t universe) const;
    std::vector<PlaylistStatus> getAllPlaylistStatuses() const;
    void clearPlaylist(universe_t universe);
    bool updatePlaylistCurrentAnimation(universe_t universe, const std::string &animationId);

  private:
    struct UniverseState {
        std::vector<std::shared_ptr<PlaybackSession>> activeSessions;
        bool isPlaylist{false};
        bool isInterrupted{false};
        bool isStopped{false}; // Explicitly stopped, will not resume
        bool shouldResumePlaylist{false};

        // Playlist state for resumption
        std::string playlistId;
        size_t currentPlaylistIndex{0};
        std::optional<PlaylistStatus> playlistStatus;
    };

    mutable std::mutex mutex_;
    std::map<universe_t, UniverseState> universeStates_;
};

} // namespace creatures
