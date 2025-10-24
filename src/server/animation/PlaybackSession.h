#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "model/Animation.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"

namespace creatures {

// Forward declarations
class OperationSpan;

/**
 * TrackState - Per-track playback state
 *
 * Holds decoded DMX frames and playback position for a single track
 */
struct TrackState {
    creatureId_t creatureId;                         // Creature this track controls
    std::vector<std::vector<uint8_t>> decodedFrames; // Decoded DMX data for each frame
    uint32_t currentFrameIndex{0};                   // Current playback position
    framenum_t nextDispatchFrame{0};                 // Next event loop frame to emit on

    [[nodiscard]] bool isFinished() const { return currentFrameIndex >= decodedFrames.size(); }
    [[nodiscard]] uint32_t getTotalFrames() const { return static_cast<uint32_t>(decodedFrames.size()); }
};

/**
 * PlaybackSession - Manages the state and lifetime of a single animation playback
 *
 * This class replaces the legacy bulk-scheduling approach with a cooperative
 * playback pipeline that allows instant cancellation and interactive overrides.
 *
 * Key responsibilities:
 * - Hold decoded DMX frame buffers for each track
 * - Track audio timeline and stream buffer
 * - Manage per-track frame indices and next execution times
 * - Provide cancellation mechanism
 * - Maintain observability spans throughout playback
 *
 * Lifecycle:
 * 1. Created by PlaylistEvent or direct animation play requests
 * 2. Initialized with decoded animation data and audio buffer
 * 3. Referenced by PlaybackRunnerEvent for each frame slice
 * 4. Cancelled externally via cancel() or runs to natural completion
 * 5. Cleaned up when last reference is released
 */
class PlaybackSession {
  public:
    /**
     * Create a new playback session for an animation
     *
     * @param animation The animation to play
     * @param universe The DMX universe to output on
     * @param startingFrame The frame number when playback begins
     * @param parentSpan Optional parent observability span
     */
    PlaybackSession(const Animation &animation, universe_t universe, framenum_t startingFrame,
                    std::shared_ptr<OperationSpan> parentSpan = nullptr);

    ~PlaybackSession();

    // Non-copyable, non-movable (due to atomic member)
    PlaybackSession(const PlaybackSession &) = delete;
    PlaybackSession &operator=(const PlaybackSession &) = delete;
    PlaybackSession(PlaybackSession &&) = delete;
    PlaybackSession &operator=(PlaybackSession &&) = delete;

    /**
     * Cancel this playback session
     *
     * Sets the cancelled flag, which causes the PlaybackRunnerEvent to:
     * 1. Perform teardown (DMX blackout, status light off, stop audio)
     * 2. Not reschedule itself
     *
     * Thread-safe: Can be called from any thread
     */
    void cancel();

    /**
     * Check if this session has been cancelled
     *
     * @return true if cancel() has been called
     */
    [[nodiscard]] bool isCancelled() const { return cancelled_.load(); }

    /**
     * Get the animation being played
     */
    [[nodiscard]] const Animation &getAnimation() const { return animation_; }

    /**
     * Get the universe this session is playing on
     */
    [[nodiscard]] universe_t getUniverse() const { return universe_; }

    /**
     * Get the starting frame number
     */
    [[nodiscard]] framenum_t getStartingFrame() const { return startingFrame_; }

    /**
     * Set the starting frame number
     *
     * Used when adjusting the start time after audio loading completes
     * Also updates all track states to dispatch at the new starting frame
     */
    void setStartingFrame(framenum_t frame);

    /**
     * Get the milliseconds per frame for this animation
     */
    [[nodiscard]] uint32_t getMsPerFrame() const { return animation_.metadata.milliseconds_per_frame; }

    /**
     * Get the observability span for this session
     */
    [[nodiscard]] std::shared_ptr<OperationSpan> getSpan() const { return sessionSpan_; }

    /**
     * Get the audio stream buffer (may be nullptr if no audio)
     */
    [[nodiscard]] std::shared_ptr<rtp::AudioStreamBuffer> getAudioBuffer() const { return audioBuffer_; }

    /**
     * Set the audio stream buffer
     *
     * Called by audio loading code after buffer is prepared
     */
    void setAudioBuffer(std::shared_ptr<rtp::AudioStreamBuffer> buffer) { audioBuffer_ = buffer; }

    /**
     * Get the audio transport (may be nullptr if no audio or not yet set)
     */
    [[nodiscard]] std::shared_ptr<class AudioTransport> getAudioTransport() const { return audioTransport_; }

    /**
     * Set the audio transport
     *
     * Called by scheduler when setting up playback
     */
    void setAudioTransport(std::shared_ptr<class AudioTransport> transport) { audioTransport_ = transport; }

    /**
     * Register a callback to be invoked when playback starts
     */
    void setOnStartCallback(std::function<void()> callback) { onStart_ = std::move(callback); }

    /**
     * Register a callback to be invoked when playback finishes (naturally or cancelled)
     */
    void setOnFinishCallback(std::function<void()> callback) { onFinish_ = std::move(callback); }

    /**
     * Invoke the start callback (called by PlaybackRunnerEvent on first execution)
     */
    void invokeOnStart() {
        if (onStart_) {
            onStart_();
        }
    }

    /**
     * Invoke the finish callback (called by PlaybackRunnerEvent on completion or cancellation)
     */
    void invokeOnFinish() {
        if (onFinish_) {
            onFinish_();
        }
    }

    /**
     * Get track states (for DMX emission)
     */
    [[nodiscard]] std::vector<TrackState> &getTrackStates() { return trackStates_; }

    /**
     * Get track states (const version)
     */
    [[nodiscard]] const std::vector<TrackState> &getTrackStates() const { return trackStates_; }

  private:
    // Animation and playback metadata
    Animation animation_;
    universe_t universe_;
    framenum_t startingFrame_;

    // Per-track decoded frames and playback state
    std::vector<TrackState> trackStates_;

    // Audio buffer (if animation has sound)
    std::shared_ptr<rtp::AudioStreamBuffer> audioBuffer_;

    // Audio transport for playback
    std::shared_ptr<class AudioTransport> audioTransport_;

    // Cancellation flag (atomic for thread-safety)
    std::atomic<bool> cancelled_{false};

    // Lifecycle callbacks
    std::function<void()> onStart_;
    std::function<void()> onFinish_;

    // Observability
    std::shared_ptr<OperationSpan> sessionSpan_;
};

} // namespace creatures
