
#pragma once

#include <queue>
#include <memory>
#include <mutex>

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

        [[nodiscard]] uint64_t getCurrentFrameNumber() const;
        uint64_t getNextFrameNumber() const;
        uint32_t getQueueSize() const;
        uint64_t getEventsExecuted() const;

    private:

        void main_loop();

        std::thread eventLoopThread;

        uint64_t frameCount = 0;
        uint64_t eventsExecuted = 0;

        std::unique_ptr<EventScheduler> eventScheduler;
        std::mutex eventQueueMutex;
    };

}