
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

} // namespace creatures
