//
// event.h   — thread-safe friendly, now with RAII-correct destructors
//
#pragma once

#include <memory>
#include <queue>

#include "server/namespace-stuffs.h"

namespace creatures {

    /**
     * Base class for the items in the event queue
     */
    class Event {
    public:
        explicit Event(framenum_t frame) : frameNumber(frame) {}

        // ★ The magic line that fixes the warning:
        virtual ~Event() = default;

        virtual void execute() = 0;

        framenum_t frameNumber = 0;
    };

    // Curiously Recurring Template Pattern (CRTP)
    template <typename Derived>
    class EventBase : public Event {
    public:
        explicit EventBase(framenum_t frame) : Event(frame) {}

        // virtual because Event has one; =default makes it explicit & cheap
        ~EventBase() override = default;

        void execute() override {
            static_cast<Derived*>(this)->executeImpl();
        }
    };

    struct EventComparator {
        bool operator()(const std::shared_ptr<Event>& e1,
                        const std::shared_ptr<Event>& e2) const {
            return e1->frameNumber > e2->frameNumber;
        }
    };

    class EventScheduler {
    public:
        EventScheduler() = default;

        // You may still want a mutex/cond-var if other threads push events 💡
        std::priority_queue<
            std::shared_ptr<Event>,
            std::vector<std::shared_ptr<Event>>,
            EventComparator
        > event_queue;
    };

} // namespace creatures