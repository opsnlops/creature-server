
#pragma once

#include <memory>
#include <queue>



namespace creatures {

    /**
     * Base class for the items in the event queue
     */
    class Event {
    public:
        explicit Event(uint64_t frame) : frameNumber(frame) {}
        virtual void execute() = 0;

        uint64_t frameNumber = 0;
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