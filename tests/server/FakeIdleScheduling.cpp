#include "server/animation/CooperativeAnimationScheduler.h"
#include "server/animation/SessionManager.h"
#include "server/animation/player.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "util/Result.h"

namespace creatures {

extern std::shared_ptr<SessionManager> sessionManager;

// Constructible-but-inert EventLoop so tests can satisfy the non-null
// eventLoop guard in SessionManager::interrupt (issue #100). Never started.
EventLoop::EventLoop() = default;
void EventLoop::start() {}
void EventLoop::run() {}

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

// The real SessionManager (issue #100 ownership tests) references these; the tests
// only exercise registration/queue/abort paths, which never reach them.
PlaybackRunnerEvent::PlaybackRunnerEvent(framenum_t frameNumber, std::shared_ptr<PlaybackSession> session)
    : EventBase(frameNumber), session_(std::move(session)) {}

Result<framenum_t> PlaybackRunnerEvent::executeImpl() { return Result<framenum_t>{0}; }

// Minimal fake of the real scheduler's adoption step: create the session and
// register it, exactly like scheduleAnimation does before any audio work. No
// audio load, no runner, no broadcasts — just enough for SessionManager's
// interrupt() paths to be unit-testable (issue #100).
Result<std::shared_ptr<PlaybackSession>>
CooperativeAnimationScheduler::scheduleAnimation(framenum_t startingFrame, const Animation &animation,
                                                 universe_t universe, creatures::runtime::ActivityReason reason,
                                                 bool cancelEntireUniverse, const std::string &chainId) {
    auto session = std::make_shared<PlaybackSession>(animation, universe, startingFrame, nullptr);
    session->setActivityReason(reason);
    session->setChainId(chainId);
    if (session->isCancelled()) {
        return Result<std::shared_ptr<PlaybackSession>>{
            ServerError(ServerError::InvalidData, "test session failed to decode frames")};
    }
    if (sessionManager) {
        sessionManager->registerSession(universe, session, false, nullptr, cancelEntireUniverse);
    }
    return Result<std::shared_ptr<PlaybackSession>>{session};
}

} // namespace creatures
