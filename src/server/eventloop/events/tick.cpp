

#include "spdlog/spdlog.h"

#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

#define TICK_TIME_FRAMES 10000

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;

    void TickEvent::execute() {

        // Just go tick!
        debug("⌚️ tick");

        // Make another event
        auto nextTick = std::make_shared<TickEvent>(this->frameNumber + TICK_TIME_FRAMES);
        eventLoop->scheduleEvent(nextTick);
    }

}