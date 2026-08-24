#pragma once

#include <optional>

#include "PlaybackSession.h"
#include "model/Animation.h"
#include "server/namespace-stuffs.h"
#include "server/rtp/AudioLoadExecutor.h"
#include "util/Result.h"

namespace creatures {

/**
 * CooperativeAnimationScheduler - Cooperative playback scheduler
 *
 * This scheduler creates PlaybackSession objects and uses PlaybackRunnerEvent
 * for frame-by-frame cooperative playback instead of bulk-scheduling thousands
 * of events upfront.
 *
 * Key properties:
 * - Instant cancellation via session->cancel()
 * - Shallow event queue (only runner + current frame)
 * - Interactive overrides possible during playback
 * - Clean separation of DMX and audio transport
 * - Full observability maintained
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
     * 5. Creates the appropriate RTP or native-local AudioTransport
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
     * @param chainId Stable chain id for chained speech playback (issue #100) —
     *                stamped on the session before publication so SessionManager can
     *                scope queue pops and failure cleanup to the owning chain
     * @return Playback session handle for external control, or error
     */
    static Result<std::shared_ptr<PlaybackSession>>
    scheduleAnimation(framenum_t startingFrame, const Animation &animation, universe_t universe,
                      creatures::runtime::ActivityReason reason = creatures::runtime::ActivityReason::Play,
                      bool cancelEntireUniverse = false, const std::string &chainId = {},
                      std::optional<uint64_t> expectedPlaylistGeneration = std::nullopt);

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
     * @return RTP or native-local AudioTransport instance
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
     * Submit the RTP audio buffer load to the fixed, bounded executor, then schedule
     * the encoder reset and initial PlaybackRunnerEvent from its worker (issues #70/#95).
     * The session must already be adopted, broadcast as running, and have its callbacks
     * set. Admission failure is synchronous and fully unwinds the adopted session.
     *
     * @param session The adopted playback session
     * @param universe DMX universe (for unwind bookkeeping)
     * @param scheduleSpan The schedule span; its trace/span ids are stamped on the
     *                     worker's root span for Honeycomb linkage
     * @return Success when admitted, or an explicit overload/shutdown error
     */
    static Result<void> scheduleWithAsyncAudioLoad(std::shared_ptr<PlaybackSession> session, universe_t universe,
                                                   std::shared_ptr<class OperationSpan> scheduleSpan,
                                                   const std::shared_ptr<rtp::AudioLoadExecutor> &executor,
                                                   rtp::AudioLoadExecutor::Reservation reservation);
};

} // namespace creatures
