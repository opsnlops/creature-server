#pragma once

#include "PlaybackSession.h"
#include "model/Animation.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * CooperativeAnimationScheduler - Modern cooperative playback scheduler
 *
 * This scheduler creates PlaybackSession objects and uses PlaybackRunnerEvent
 * for frame-by-frame cooperative playback instead of bulk-scheduling thousands
 * of events upfront.
 *
 * Key advantages over LegacyAnimationScheduler:
 * - Instant cancellation via session->cancel()
 * - Shallow event queue (only runner + current frame)
 * - Interactive overrides possible during playback
 * - Clean separation of DMX and audio transport
 * - Full observability maintained
 *
 * This is the future-proof implementation that will eventually replace the legacy
 * bulk scheduler once validated.
 */
class CooperativeAnimationScheduler {
  public:
    /**
     * Schedule an animation using the cooperative playback approach
     *
     * This method:
     * 1. Creates a PlaybackSession with the animation data
     * 2. Loads and decodes audio buffer if present
     * 3. Creates appropriate AudioTransport (RTP or SDL)
     * 4. Sets up lifecycle callbacks (status lights, metrics)
     * 5. Schedules initial PlaybackRunnerEvent
     *
     * @param startingFrame Frame number to start the animation
     * @param animation Animation to schedule
     * @param universe DMX universe to play on
     * @return Playback session handle for external control, or error
     */
    static Result<std::shared_ptr<PlaybackSession>> scheduleAnimation(framenum_t startingFrame,
                                                                      const Animation &animation, universe_t universe);

  private:
    // No instances needed - all static methods
    CooperativeAnimationScheduler() = delete;

    /**
     * Load audio buffer for the animation
     *
     * @param animation The animation with sound file metadata
     * @param session The playback session to populate
     * @param parentSpan Observability span for tracing
     * @return Success or error
     */
    static Result<void> loadAudioBuffer(const Animation &animation, std::shared_ptr<PlaybackSession> session,
                                        std::shared_ptr<class OperationSpan> parentSpan);

    /**
     * Create appropriate audio transport for the configuration
     *
     * @param session The playback session
     * @return AudioTransport instance (RTP or SDL)
     */
    static std::shared_ptr<class AudioTransport> createAudioTransport(std::shared_ptr<PlaybackSession> session);

    /**
     * Set up lifecycle callbacks for the session
     *
     * @param session The playback session
     * @param universe DMX universe
     */
    static void setupLifecycleCallbacks(std::shared_ptr<PlaybackSession> session, universe_t universe);
};

} // namespace creatures
