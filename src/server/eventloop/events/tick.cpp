
#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;

    void TickEvent::executeImpl() {

        // Just go tick!
        debug("⌚️ Hello from frame {:L}! Event queue length: {}, events executed: {:L}",
              eventLoop->getCurrentFrameNumber(),
              eventLoop->getQueueSize(),
              eventLoop->getEventsExecuted());

        // Make another event
        auto nextTick = std::make_shared<TickEvent>(this->frameNumber + TICK_TIME_FRAMES);
        eventLoop->scheduleEvent(nextTick);
    }

}