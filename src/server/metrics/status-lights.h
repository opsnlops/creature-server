
#pragma once

#include "server/metrics/counters.h"
#include "server/gpio/gpio.h"


namespace creatures {

    /**
     * This runs in a thread and updates the status lights
     *
     * It runs in a dedicated thread because it also monitors the event queue. I decided
     * to not have it run as an event, because then it wouldn't be able to detect stalls in
     * the event queue.
     */
    class StatusLights {

    public:
        StatusLights();
        ~StatusLights() = default;

        void run();
        void stop();

    private:

        uint64_t lastFrameSeen;
        uint64_t lastDmxEventSeen;
        uint64_t lastStreamedFrameSeen;

        bool runningLightOn;
        bool dmxEventLightOn;
        bool streamingLightOn;

        bool shouldRun;

    };

}