
#pragma once

#include <vector>

#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"


namespace creatures {

    class TickEvent : public EventBase<TickEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();
    };

    class DMXEvent : public EventBase<DMXEvent> {
    public:
        using EventBase::EventBase;
        void executeImpl();

        // Used to create a DMX object if it doesn't exist
        std::string clientIP;
        uint32_t dmxUniverse;
        uint32_t dmxOffset;
        uint32_t numMotors;

        // Used every time to send data
        std::vector<uint8_t> data;
        uint8_t count;
    };


}