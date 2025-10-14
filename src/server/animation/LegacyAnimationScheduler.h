#pragma once

#include "model/Animation.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * LegacyAnimationScheduler - Reference implementation of bulk event scheduling
 *
 * This class encapsulates the original animation scheduling logic where all
 * DMX frames and audio chunks are scheduled upfront as individual events.
 *
 * This is being extracted as a reference implementation before migration to
 * the new PlaybackSession/PlaybackRunner architecture that supports
 * cancellation and interactive overrides.
 *
 * IMPORTANT: This scheduler maintains full observability via ObservabilityManager
 *
 * Known Limitations (by design):
 * - Floods event queue with all frames upfront
 * - No cancellation mechanism once scheduled
 * - Cannot handle interactive overrides
 * - Audio scheduling creates additional bulk events
 */
class LegacyAnimationScheduler {
  public:
    /**
     * Schedule an animation using the legacy bulk-scheduling approach
     *
     * This method:
     * 1. Validates all creatures in the animation exist (with observability)
     * 2. Schedules audio playback event (if sound file present)
     * 3. Schedules status light ON event
     * 4. Bulk schedules all DMX frame events (one per frame per track)
     * 5. Schedules status light OFF event
     * 6. Increments animation counter metrics
     *
     * @param startingFrame Frame number to start the animation
     * @param animation Animation to schedule
     * @param universe DMX universe to play on
     * @return Last frame number of the animation, or error
     */
    static Result<framenum_t> scheduleAnimation(framenum_t startingFrame, const Animation &animation,
                                                universe_t universe);

  private:
    // No instances needed - all static methods
    LegacyAnimationScheduler() = delete;
};

} // namespace creatures
