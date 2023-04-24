
#pragma once

#include "spdlog/spdlog.h"


#define EVENT_LOOP_PERIOD_MS    10000


namespace creatures {

    class EventLoop {

    public:
        EventLoop();
        ~EventLoop();

        static void main_loop();

    };

}