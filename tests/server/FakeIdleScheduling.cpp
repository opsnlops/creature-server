#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "util/Result.h"

namespace creatures {

framenum_t EventLoop::getNextFrameNumber() const { return 0; }

// Stubs to satisfy the linker for tests that include FixturePatternRunner.cpp.
// Never invoked by the live-control unit tests (which only call setLive()/hasLive());
// the symbols just need to resolve because the runner's other methods reference them.
void EventLoop::scheduleEvent(const std::shared_ptr<Event> & /*e*/) {}
Result<framenum_t> DMXEvent::executeImpl() { return Result<framenum_t>{0}; }

Result<framenum_t> scheduleAnimation(framenum_t /*startingFrame*/, const Animation & /*animation*/,
                                     universe_t /*universe*/, creatures::runtime::ActivityReason /*reason*/) {
    return Result<framenum_t>{0};
}

bool SessionManager::hasActiveSessionForCreature(universe_t /*universe*/, const creatureId_t & /*creatureId*/) const {
    return false;
}

bool SessionManager::cancelIdleSessionForCreature(universe_t /*universe*/, const creatureId_t & /*creatureId*/) {
    return false;
}

bool SessionManager::hasActiveNonIdleSession(universe_t /*universe*/) const { return false; }

} // namespace creatures
