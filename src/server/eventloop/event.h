//
// event.h
//
#pragma once

#include <memory>
#include <queue>
#include <mutex>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

    /**
     * Base class for the items in the event queue
     *
     * Events now return Result<framenum_t> to indicate success/failure
     * and optionally communicate the last frame they processed.
     */
    class Event {
    public:
        explicit Event(framenum_t frame) : frameNumber(frame) {}

        // Virtual destructor - essential for proper cleanup! 🐰
        virtual ~Event() = default;

        /**
         * Execute this event
         * @return Result containing the frame number when complete, or an error
         */
        virtual Result<framenum_t> execute() = 0;

        framenum_t frameNumber = 0;
    };

    /**
     * Curiously Recurring Template Pattern (CRTP) base class
     *
     * This handles the boilerplate of calling the derived class's executeImpl()
     * method while maintaining type safety and performance.
     */
    template <typename Derived>
    class EventBase : public Event {
    public:
        explicit EventBase(framenum_t frame) : Event(frame) {}

        // Virtual destructor override
        ~EventBase() override = default;

        /**
         * Calls the derived class's executeImpl() method
         * @return Result from the derived implementation
         */
        Result<framenum_t> execute() override {
            return static_cast<Derived*>(this)->executeImpl();
        }
    };

    /**
     * Comparator for the priority queue
     *
     * Events with smaller frame numbers have higher priority
     * (they should be executed first)
     */
    struct EventComparator {
        bool operator()(const std::shared_ptr<Event>& e1,
                        const std::shared_ptr<Event>& e2) const {
            return e1->frameNumber > e2->frameNumber;
        }
    };

    /**
     * Event scheduler using a priority queue
     *
     * Thread safety note: If you're scheduling events from multiple threads,
     * you'll want to add mutex protection around the queue operations.
     */
    class EventScheduler {
    public:
        EventScheduler() = default;

        // Non-copyable to avoid accidental queue duplication
        EventScheduler(const EventScheduler&) = delete;
        EventScheduler& operator=(const EventScheduler&) = delete;

        // Move operations are fine
        EventScheduler(EventScheduler&&) = default;
        EventScheduler& operator=(EventScheduler&&) = default;

        /**
         * Priority queue that keeps events sorted by frame number
         * Events with smaller frame numbers will be at the top
         */
        std::priority_queue<
            std::shared_ptr<Event>,
            std::vector<std::shared_ptr<Event>>,
            EventComparator
        > event_queue;

        // If you need thread safety for multi-threaded event scheduling:
        // std::mutex queue_mutex;
        // std::condition_variable queue_condition;
    };

} // namespace creatures