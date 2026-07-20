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
     * 2. Adopts the session via SessionManager::registerSession — cancels conflicting
     *    sessions and registers the new one atomically, BEFORE the running broadcast
     *    and audio load (issues #62/#63)
     * 3. Broadcasts the (reason, running) activity state
     * 4. Loads and decodes audio buffer if present
     * 5. Creates appropriate AudioTransport (RTP or SDL)
     * 6. Sets up lifecycle callbacks (status lights, metrics)
     * 7. Schedules initial PlaybackRunnerEvent
     *
     * Callers must NOT register the returned session themselves — adoption already did.
     *
     * RTP audio note (issue #70): for sound-bearing animations in RTP mode, this returns
     * as soon as the session is adopted and broadcast — the WAV read + Opus encode runs on
     * a background worker, which schedules the PlaybackRunnerEvent when the buffer is
     * ready. Playback therefore starts a load-duration after this returns, and audio
     * failures surface asynchronously (session unwound, playlist halted) rather than as
     * an error Result here.
     *
     * @param startingFrame Frame number to start the animation
     * @param animation Animation to schedule
     * @param universe DMX universe to play on
     * @param reason Activity reason broadcast for the involved creatures
     * @param cancelEntireUniverse Adopt with interrupt semantics: cancel every active
     *                             session on the universe, not just overlapping ones
     * @return Playback session handle for external control, or error
     */
    static Result<std::shared_ptr<PlaybackSession>>
    scheduleAnimation(framenum_t startingFrame, const Animation &animation, universe_t universe,
                      creatures::runtime::ActivityReason reason = creatures::runtime::ActivityReason::Play,
                      bool cancelEntireUniverse = false);

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

    /**
     * Load the RTP audio buffer on a detached worker thread, then schedule the encoder
     * reset and initial PlaybackRunnerEvent from there (issue #70). The session must
     * already be adopted, broadcast as running, and have its callbacks set — the worker
     * only loads, adjusts the start frame, and schedules (or unwinds on failure).
     *
     * @param session The adopted playback session
     * @param universe DMX universe (for unwind bookkeeping)
     * @param scheduleSpan The schedule span; its trace/span ids are stamped on the
     *                     worker's root span for Honeycomb linkage
     */
    static void scheduleWithAsyncAudioLoad(std::shared_ptr<PlaybackSession> session, universe_t universe,
                                           std::shared_ptr<class OperationSpan> scheduleSpan);
};

} // namespace creatures
