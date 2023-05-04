
#pragma once

#include <queue>
#include <memory>
#include <mutex>
#include <thread>

#include "spdlog/spdlog.h"

#include "server/eventloop/event.h"


namespace creatures {

    class EventLoop {

    public:
        EventLoop();
        ~EventLoop();

        void run();

        void scheduleEvent(const std::shared_ptr<Event>& e);

        [[nodiscard]] uint64_t getCurrentFrameNumber() const;
        [[nodiscard]] uint64_t getNextFrameNumber() const;
        [[nodiscard]] uint32_t getQueueSize() const;
        [[nodiscard]] uint64_t getEventsExecuted() const;

    private:

        void main_loop();

        std::thread eventLoopThread;

        uint64_t frameCount = 0;
        uint64_t eventsExecuted = 0;

        std::unique_ptr<EventScheduler> eventScheduler;
        std::mutex eventQueueMutex;
    };

}