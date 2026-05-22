
#pragma once

#include "server/eventloop/event.h"
#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * Self-scheduling event that drives `FixturePatternRunner::tick()` at ~50 Hz.
 *
 * Reschedules itself every `FIXTURE_PATTERN_TICK_INTERVAL_FRAMES` while any patterns are
 * active; stops chaining once the runner reports an empty active set. The
 * `FixtureBindingDispatcher` re-arms a tick whenever it starts a pattern.
 */
class FixturePatternTickEvent : public EventBase<FixturePatternTickEvent> {
  public:
    using EventBase::EventBase;
    virtual ~FixturePatternTickEvent() = default;

    Result<framenum_t> executeImpl();
};

// 20 frames @ 1ms = 20ms ≈ 50 Hz. Matches DMX refresh (~44 Hz) and what stage lighting
// consoles use. Faster is wasted CPU on values no receiver can show.
constexpr framenum_t FIXTURE_PATTERN_TICK_INTERVAL_FRAMES = 20;

// 0.05% — matches DEFAULT_EVENT_LOOP_TRACE_SAMPLING and PLAYBACK_RUNNER_TRACE_SAMPLING.
// At 50 Hz that's ~1.5 sampled traces/min when patterns are active, ~0 when idle.
constexpr double FIXTURE_PATTERN_TICK_TRACE_SAMPLING = 0.0005;

} // namespace creatures
