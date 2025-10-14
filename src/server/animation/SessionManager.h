#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "PlaybackSession.h"
#include "model/Animation.h"
#include "server/namespace-stuffs.h"

namespace creatures {

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
     * If there's an existing session on the same universe, it will be cancelled.
     *
     * @param universe The universe this session is playing on
     * @param session The playback session
     * @param isPlaylist True if this session is part of a playlist
     */
    void registerSession(universe_t universe, std::shared_ptr<PlaybackSession> session, bool isPlaylist = false);

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
                                                       bool shouldResumePlaylist = false);

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
     * Check if a universe is currently playing
     *
     * @param universe The universe to check
     * @return True if there's an active session on this universe
     */
    bool isPlaying(universe_t universe) const;

    /**
     * Check if a universe has a paused playlist that can be resumed
     *
     * @param universe The universe to check
     * @return True if there's a paused playlist
     */
    bool hasInterruptedPlaylist(universe_t universe) const;

  private:
    struct UniverseState {
        std::shared_ptr<PlaybackSession> currentSession;
        bool isPlaylist{false};
        bool isInterrupted{false};
        bool shouldResumePlaylist{false};

        // Playlist state for resumption
        std::string playlistId;
        size_t currentPlaylistIndex{0};
    };

    mutable std::mutex mutex_;
    std::map<universe_t, UniverseState> universeStates_;
};

} // namespace creatures
