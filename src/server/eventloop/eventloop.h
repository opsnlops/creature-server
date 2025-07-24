//
// eventloop.h
//

#pragma once

#include <memory>
#include <mutex>
#include <queue>
#include <thread>

#include "spdlog/spdlog.h"

#include "server/eventloop/event.h"
#include "util/StoppableThread.h"

namespace creatures {

class EventLoop final : public StoppableThread {

  public:
    EventLoop();
    ~EventLoop() = default;

    void scheduleEvent(const std::shared_ptr<Event> &e);

    [[nodiscard]] framenum_t getCurrentFrameNumber() const;
    [[nodiscard]] framenum_t getNextFrameNumber() const;
    [[nodiscard]] uint32_t getQueueSize() const;

    void start() override;

  protected:
    void run() override;

  private:
    framenum_t frameCount = 0;

    std::unique_ptr<EventScheduler> eventScheduler;
    std::mutex eventQueueMutex;
};

} // namespace creatures