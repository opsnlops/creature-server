#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#include "model/Animation.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/runtime/Activity.h"
#include "util/uuidUtils.h"

namespace creatures {

class OperationSpan;

/**
 * StreamingPlaybackSession
 *
 * Extends the playback session concept for streaming scenarios where DMX frames
 * and audio arrive incrementally from the ElevenLabs WebSocket stream rather than
 * being fully available upfront.
 *
 * Key differences from PlaybackSession:
 * - Frame buffer is growable: new frames can be appended while playback is running
 * - Audio buffer can be fed incrementally
 * - Tracks "waiting for data" state when the runner catches up to available frames
 * - Base animation frames are loaded upfront (they're small, already in DB)
 * - As alignment chunks arrive: mouth frames are generated and appended
 *
 * Thread safety:
 * - appendFrames() is called from the WebSocket receive thread
 * - The event loop reads frames from the runner thread
 * - A mutex protects the growable buffer
 * - Keep critical sections minimal: push to vector + increment counter
 */
class StreamingPlaybackSession {
  public:
    /**
     * Create a streaming playback session.
     *
     * @param animation The base animation (used for non-mouth servo values)
     * @param universe The DMX universe to output on
     * @param startingFrame Event loop frame number when playback begins
     * @param parentSpan Optional observability span
     */
    StreamingPlaybackSession(const Animation &animation, universe_t universe, framenum_t startingFrame,
                              std::shared_ptr<OperationSpan> parentSpan = nullptr);

    ~StreamingPlaybackSession();

    // Non-copyable, non-movable
    StreamingPlaybackSession(const StreamingPlaybackSession &) = delete;
    StreamingPlaybackSession &operator=(const StreamingPlaybackSession &) = delete;

    /**
     * Append new DMX frames to the buffer.
     *
     * Thread-safe. Called from the WebSocket receive thread as alignment
     * chunks are processed into animation frames.
     *
     * @param creatureId Which creature track these frames belong to
     * @param newFrames Vector of DMX frame data to append
     */
    void appendFrames(const creatureId_t &creatureId, const std::vector<std::vector<uint8_t>> &newFrames);

    /**
     * Signal that no more frames will arrive.
     *
     * Called when the ElevenLabs stream ends. After this, the runner will
     * finish naturally when all frames are consumed.
     */
    void markComplete();

    /**
     * Check if the stream is complete (no more frames coming).
     */
    [[nodiscard]] bool isComplete() const { return complete_.load(); }

    /**
     * Check if the runner has caught up to available frames.
     *
     * When true, the runner should hold the last frame (not jerk to rest)
     * and reschedule for the next frame period.
     */
    [[nodiscard]] bool isWaitingForData(const creatureId_t &creatureId) const;

    /**
     * Get the number of frames currently available for a creature.
     */
    [[nodiscard]] uint32_t availableFrames(const creatureId_t &creatureId) const;

    /**
     * Get the current frame index for a creature.
     */
    [[nodiscard]] uint32_t currentFrameIndex(const creatureId_t &creatureId) const;

    /**
     * Get a frame at the given index for a creature.
     * Returns empty vector if index is out of range.
     */
    std::vector<uint8_t> getFrame(const creatureId_t &creatureId, uint32_t frameIndex) const;

    /**
     * Advance the playback position for a creature.
     */
    void advanceFrame(const creatureId_t &creatureId);

    /**
     * Check if all tracks have finished playback.
     */
    [[nodiscard]] bool isAllTracksFinished() const;

    // --- Standard session interface (matches PlaybackSession API) ---

    void cancel();
    [[nodiscard]] bool isCancelled() const { return cancelled_.load(); }
    [[nodiscard]] const Animation &getAnimation() const { return animation_; }
    [[nodiscard]] universe_t getUniverse() const { return universe_; }
    [[nodiscard]] framenum_t getStartingFrame() const { return startingFrame_; }
    void setStartingFrame(framenum_t frame) { startingFrame_ = frame; }
    [[nodiscard]] uint32_t getMsPerFrame() const { return animation_.metadata.milliseconds_per_frame; }

    [[nodiscard]] creatures::runtime::ActivityReason getActivityReason() const { return activityReason_; }
    void setActivityReason(creatures::runtime::ActivityReason reason) { activityReason_ = reason; }

    void markCancellationNotified() { cancellationNotified_ = true; }
    [[nodiscard]] bool isCancellationNotified() const { return cancellationNotified_; }

    [[nodiscard]] std::shared_ptr<OperationSpan> getSpan() const { return sessionSpan_; }

    [[nodiscard]] std::shared_ptr<rtp::AudioStreamBuffer> getAudioBuffer() const { return audioBuffer_; }
    void setAudioBuffer(std::shared_ptr<rtp::AudioStreamBuffer> buffer) { audioBuffer_ = buffer; }

    [[nodiscard]] std::shared_ptr<class AudioTransport> getAudioTransport() const { return audioTransport_; }
    void setAudioTransport(std::shared_ptr<class AudioTransport> transport) { audioTransport_ = transport; }

    void setOnStartCallback(std::function<void()> callback) { onStart_ = std::move(callback); }
    void setOnFinishCallback(std::function<void()> callback) { onFinish_ = std::move(callback); }

    bool invokeOnStart() {
        if (hasStarted_) {
            return false;
        }
        hasStarted_ = true;
        if (onStart_) {
            onStart_();
        }
        return true;
    }

    void invokeOnFinish() {
        if (onFinish_) {
            onFinish_();
        }
    }

    [[nodiscard]] const std::string &getSessionId() const { return sessionId_; }

  private:
    /**
     * Per-creature streaming track state.
     */
    struct StreamingTrackState {
        creatureId_t creatureId;
        std::vector<std::vector<uint8_t>> frames; // Growable frame buffer
        uint32_t playbackIndex = 0;                // Current playback position
    };

    // Mutex protecting the track state vectors
    mutable std::mutex trackMutex_;

    // Track states indexed by creature ID
    std::vector<StreamingTrackState> trackStates_;

    // Animation and playback metadata
    Animation animation_;
    universe_t universe_;
    framenum_t startingFrame_;
    std::string sessionId_{creatures::util::generateUUID()};

    // Completion flag
    std::atomic<bool> complete_{false};

    // Cancellation
    std::atomic<bool> cancelled_{false};

    // Audio
    std::shared_ptr<rtp::AudioStreamBuffer> audioBuffer_;
    std::shared_ptr<class AudioTransport> audioTransport_;

    // Lifecycle
    bool hasStarted_{false};
    std::function<void()> onStart_;
    std::function<void()> onFinish_;

    // Observability
    std::shared_ptr<OperationSpan> sessionSpan_;

    // Activity
    creatures::runtime::ActivityReason activityReason_{creatures::runtime::ActivityReason::AdHoc};
    bool cancellationNotified_{false};

    /**
     * Find or create a track state for the given creature.
     * Must be called with trackMutex_ held.
     */
    StreamingTrackState &findOrCreateTrack(const creatureId_t &creatureId);

    /**
     * Find a track state for the given creature (const).
     * Must be called with trackMutex_ held.
     * Returns nullptr if not found.
     */
    const StreamingTrackState *findTrack(const creatureId_t &creatureId) const;
};

} // namespace creatures
