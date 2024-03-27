
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/eventloop/eventloop.h"
#include "server/metrics/counters.h"
#include "util/threadName.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<SystemCounters> metrics;

    EventLoop::EventLoop() : eventScheduler(std::make_unique<EventScheduler>()) {
        debug("event loop created");
    }


    void EventLoop::start() {

        frameCount = 0;

        debug("firing off event loop thread!");
        creatures::StoppableThread::start();
    }


    void EventLoop::run() {

        setThreadName("EventLoop::run");

        using namespace std::chrono;
        info("✨ eventloop running!");

        auto target_delta = milliseconds(EVENT_LOOP_PERIOD_MS);
        auto next_target_time = high_resolution_clock::now() + target_delta;

        while (!stop_requested.load()) {

            // Increment the frame counter for this pass
            frameCount++;
            metrics->incrementTotalFrames();

            // Process the events for this frame, if any, keeping the queue locked as short as possible.
            while (true) {
                std::shared_ptr<Event> event;

                {
                    std::unique_lock<std::mutex> lock(eventQueueMutex); // Unlocks when it goes out of scope
                    if (!eventScheduler->event_queue.empty() &&
                        eventScheduler->event_queue.top()->frameNumber <= frameCount) {
                        event = eventScheduler->event_queue.top();
                        eventScheduler->event_queue.pop();
                    } else {
                        break;
                    }
                }

                // Run the event in a try/catch, in case something happens. I don't want one bad event
                // to bring down the system. I want it to log and keep on going.
                try {

                    if (event) {
                        event->execute();
                        metrics->incrementEventsProcessed();
                    }

                } catch (const std::runtime_error &e) {
                    critical("An unhandled runtime error occurred on event {}: {}", metrics->getEventsProcessed(), e.what());
                } catch (const std::exception &e) {
                    critical("An unhandled exception was thrown on event {}: {}", metrics->getEventsProcessed(), e.what());
                } catch (...) {
                    critical("An unknown error occurred on event {}!", metrics->getEventsProcessed());
                }
            }

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

    uint64_t EventLoop::getNextFrameNumber() const {
        return frameCount + 1;
    }

    uint32_t EventLoop::getQueueSize() const {
        return eventScheduler->event_queue.size();
    }


    void EventLoop::scheduleEvent(const std::shared_ptr<Event>& e) {
        std::lock_guard<std::mutex> lock(eventQueueMutex);
        eventScheduler->event_queue.push(e);
    }

}