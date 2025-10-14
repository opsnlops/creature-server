#pragma once

#include <memory>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

// Forward declaration
class PlaybackSession;
class OperationSpan;

/**
 * AudioTransport - Abstract interface for audio playback mechanisms
 *
 * Provides a unified interface for different audio transport implementations:
 * - RTP streaming (frame-by-frame dispatch coordinated with DMX)
 * - Local SDL playback (fire-and-forget background thread)
 *
 * The PlaybackRunnerEvent uses this interface to manage audio without knowing
 * the underlying transport mechanism, enabling clean separation of concerns.
 */
class AudioTransport {
  public:
    virtual ~AudioTransport() = default;

    /**
     * Start audio playback for the given session
     *
     * Called by PlaybackRunnerEvent on first execution.
     *
     * For RTP: Prepares encoder and buffer for streaming
     * For SDL: Spawns background audio thread
     *
     * @param session The playback session (contains audio buffer, timing info)
     * @return Success or error
     */
    virtual Result<void> start(std::shared_ptr<PlaybackSession> session) = 0;

    /**
     * Stop audio playback immediately
     *
     * Called when session is cancelled or completes.
     *
     * For RTP: Stops sending frames, may send trailing silence
     * For SDL: Halts the audio thread
     */
    virtual void stop() = 0;

    /**
     * Check if this transport requires per-frame dispatch
     *
     * @return true if dispatchNextChunk() should be called each frame
     *         false if audio runs independently (SDL)
     */
    [[nodiscard]] virtual bool needsPerFrameDispatch() const = 0;

    /**
     * Dispatch the next audio chunk (for RTP streaming)
     *
     * Called by PlaybackRunnerEvent each frame when needsPerFrameDispatch() is true.
     * Sends the appropriate audio data for the current frame to the RTP server.
     *
     * @param currentFrame The current event loop frame number
     * @return Frame number when next dispatch is needed, or error
     */
    virtual Result<framenum_t> dispatchNextChunk(framenum_t currentFrame) = 0;

    /**
     * Check if audio has finished playing
     *
     * @return true if all audio has been dispatched/played
     */
    [[nodiscard]] virtual bool isFinished() const = 0;
};

} // namespace creatures
