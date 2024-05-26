
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
        virtual void execute() = 0;

        framenum_t frameNumber = 0;
    };

    // This is the Curiously Recurring Template Pattern (ç)
    template <typename Derived>
    class EventBase : public Event {
    public:
        explicit EventBase(framenum_t frame) : Event(frame) {}
        void execute() override {
            static_cast<Derived*>(this)->executeImpl();
        }
    };

    struct EventComparator {
        bool operator()(const std::shared_ptr<Event>& e1, const std::shared_ptr<Event>& e2) {
            return e1->frameNumber > e2->frameNumber;
        }
    };

    class EventScheduler {
    public:
        EventScheduler() = default;
        std::priority_queue<std::shared_ptr<Event>, std::vector<std::shared_ptr<Event>>, EventComparator> event_queue;
    };

}