
#include "FixturePatternTickEvent.h"

#include <memory>

#include <spdlog/spdlog.h>

#include "FixturePatternRunner.h"
#include "server/eventloop/eventloop.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<FixturePatternRunner> fixturePatternRunner;
extern std::shared_ptr<ObservabilityManager> observability;

Result<framenum_t> FixturePatternTickEvent::executeImpl() {

    if (!fixturePatternRunner) {
        return Result<framenum_t>{this->frameNumber};
    }

    // Sampled span — at 50 Hz with 0.05% sampling that's ~1.5 traces/min when active.
    // Run no instrumentation when observability is unavailable.
    std::shared_ptr<OperationSpan> tickSpan;
    if (observability) {
        tickSpan = observability->createSamplingSpan("fixture_pattern.tick", FIXTURE_PATTERN_TICK_TRACE_SAMPLING);
    }
    if (tickSpan) {
        tickSpan->setAttribute("frame", static_cast<int64_t>(this->frameNumber));
    }

    // Mark the tick as "fired" so a new start() can re-arm us if it lands between now and
    // our reschedule decision below.
    fixturePatternRunner->disarm();

    const bool stillActive = fixturePatternRunner->tick(this->frameNumber, tickSpan);

    if (tickSpan) {
        tickSpan->setAttribute("fixture.patterns.still_active", stillActive);
        tickSpan->setSuccess();
    }

    if (stillActive && eventLoop) {
        // Re-arm and reschedule ourselves for the next tick.
        if (fixturePatternRunner->tryArm()) {
            auto next =
                std::make_shared<FixturePatternTickEvent>(this->frameNumber + FIXTURE_PATTERN_TICK_INTERVAL_FRAMES);
            eventLoop->scheduleEvent(next);
        }
    }

    return Result<framenum_t>{this->frameNumber};
}

} // namespace creatures
