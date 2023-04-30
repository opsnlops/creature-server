
#pragma once

#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"


namespace creatures {


    class TickEvent : public Event {
    public:
        explicit TickEvent(uint64_t frame) : Event(frame) {}
        void execute() override;
    };

}