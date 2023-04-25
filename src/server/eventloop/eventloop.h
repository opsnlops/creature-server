
#pragma once

#include "spdlog/spdlog.h"

#include "server/eventloop/event.h"

#define EVENT_LOOP_PERIOD_MS    1


namespace creatures {

    class EventLoop {

    public:
        EventLoop();
        ~EventLoop();

        void run();

        void scheduleEvent(std::shared_ptr<Event> e);

        uint64_t getCurrentFrameNumber() const;

    private:

        void main_loop();

        std::thread eventLoopThread;

        uint64_t frameCount = 0;

        std::unique_ptr<EventScheduler> eventScheduler;
    };

}