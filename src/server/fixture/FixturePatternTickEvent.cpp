
#include "FixturePatternTickEvent.h"

#include <memory>

#include <spdlog/spdlog.h>

#include "FixturePatternRunner.h"
#include "server/eventloop/eventloop.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<FixturePatternRunner> fixturePatternRunner;

Result<framenum_t> FixturePatternTickEvent::executeImpl() {

    if (!fixturePatternRunner) {
        return Result<framenum_t>{this->frameNumber};
    }

    // Mark the tick as "fired" so a new start() can re-arm us if it lands between now and
    // our reschedule decision below.
    fixturePatternRunner->disarm();

    const bool stillActive = fixturePatternRunner->tick(this->frameNumber);

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
