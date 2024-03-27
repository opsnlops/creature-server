
#pragma once

#include <queue>
#include <memory>
#include <mutex>
#include <thread>

#include "spdlog/spdlog.h"

#include "server/eventloop/event.h"
#include "util/StoppableThread.h"


namespace creatures {

    class EventLoop : public StoppableThread {

    public:
        EventLoop();
        ~EventLoop() = default;

        void scheduleEvent(const std::shared_ptr<Event>& e);

        [[nodiscard]] uint64_t getCurrentFrameNumber() const;
        [[nodiscard]] uint64_t getNextFrameNumber() const;
        [[nodiscard]] uint32_t getQueueSize() const;

        void start() override;

    protected:
        void run() override;


    private:

        uint64_t frameCount = 0;

        std::unique_ptr<EventScheduler> eventScheduler;
        std::mutex eventQueueMutex;
    };

}