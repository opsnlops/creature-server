
#pragma once

#include <atomic>

#include "server/metrics/counters.h"
#include "server/gpio/gpio.h"
#include "server/config.h"
#include "util/StoppableThread.h"


namespace creatures {

    /**
     * This runs in a thread and updates the status lights
     *
     * It runs in a dedicated thread because it also monitors the event queue. I decided
     * to not have it run as an event, because then it wouldn't be able to detect stalls in
     * the event queue.
     */
    class StatusLights : public StoppableThread {

    public:
        StatusLights();
        ~StatusLights() = default;

        void start() override;

        // These are turned on and off by the event loop
        std::atomic<bool> animationLightOn;
        std::atomic<bool> soundLightOn;

    protected:
        void run() override;

        void sendUpdateToClients() const;

    private:

        framenum_t lastFrameSeen;
        framenum_t lastDmxEventSeen;
        framenum_t lastStreamedFrameSeen;

        bool runningLightOn;
        bool dmxEventLightOn;
        bool streamingLightOn;

        bool lastAnimationLightOn;
        bool lastSoundLightOn;

    };

}