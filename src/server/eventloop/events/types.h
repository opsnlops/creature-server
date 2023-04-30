
#pragma once

#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"


namespace creatures {

    class TickEvent : public EventBase<TickEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();
    };


}