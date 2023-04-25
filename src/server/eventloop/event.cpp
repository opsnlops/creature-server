
#include <memory>

#include "event.h"


namespace creatures {


    void EventScheduler::addEvent(std::shared_ptr<Event> e) {
        this->event_queue.push(e);
    }

}