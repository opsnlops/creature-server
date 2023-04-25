
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>

#include "spdlog/spdlog.h"



#include "server/eventloop/eventloop.h"


using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

extern std::atomic<bool> eventLoopRunning;


namespace creatures {

    EventLoop::EventLoop()  : eventScheduler(std::make_unique<EventScheduler>()) {
        debug("event loop created");

    }

    EventLoop::~EventLoop() {

        eventLoopThread.join();
        debug("farewell, event loop!");
    }




    void EventLoop::run() {

        frameCount = 0;

        debug("firing off event loop thread!");
        eventLoopThread = std::thread(&creatures::EventLoop::main_loop, this);
        debug("event loop thread running!");
    }


    void EventLoop::main_loop() {

        using namespace std::chrono;
        info("✨ eventloop running!");

        auto target_delta = milliseconds(EVENT_LOOP_PERIOD_MS); // defined in eventloop.h
        auto next_target_time = high_resolution_clock::now() + target_delta;

        while (eventLoopRunning) {

            // Increment the frame counter for this pass
            frameCount++;

            //debug("⌚️ tick");








            // Figure out how much time we have until the next tick
            auto remaining_time = next_target_time - high_resolution_clock::now();

            // If there's time left, wait.
            if (remaining_time > nanoseconds(0)) {
                // Sleep for the remaining time
                std::this_thread::sleep_for(remaining_time);
            }

            // Update the target time for the next iteration
            next_target_time += target_delta;
        }

        info("👋🏻 event loop stopped");

    }


    uint64_t EventLoop::getCurrentFrameNumber() const {
        return frameCount;
    }

    void EventLoop::scheduleEvent(std::shared_ptr<Event> e) {
        eventScheduler->event_queue.push(e);
    }

}