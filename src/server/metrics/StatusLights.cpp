
#include "spdlog/spdlog.h"

#include "server/gpio/gpio.h"
#include "server/metrics/counters.h"
#include "server/metrics/StatusLights.h"
#include "util/threadName.h"


#include "server/config.h"
#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<GPIO> gpioPins;


    StatusLights::StatusLights() {

        lastFrameSeen = 0;
        lastDmxEventSeen = 0;
        lastStreamedFrameSeen = 0;

        runningLightOn = false;
        dmxEventLightOn = false;
        streamingLightOn = false;

        // Start by turning off all the lights
        gpioPins->serverOnline(runningLightOn);
        gpioPins->sendingDMX(dmxEventLightOn);
        gpioPins->receivingStreamFrames(streamingLightOn);

        debug("StatusLights created!");
    }

    void StatusLights::start() {
        info("starting the status lights thread");
        creatures::StoppableThread::start();
    }

    void StatusLights::run() {

        setThreadName("StatusLights::run");

        debug("starting the status light loop");


        // Set up the counters to the current state
        lastDmxEventSeen = metrics->getDMXEventsProcessed();
        lastFrameSeen = metrics->getTotalFrames();
        lastStreamedFrameSeen = metrics->getFramesStreamed();

        while(!stop_requested.load()) {


            // This one doesn't need to be as precise as the others. It's okay to stop for
            // the config value. A few missed milliseconds is meaningless here.
            std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_THREAD_PERIOD_MS));

            // Make sure the pointers we touch are valid
            if(metrics && gpioPins) {

                // Most important one first, let's see if frames are progressing
                if (lastFrameSeen <= metrics->getTotalFrames()) {

                    // Time has progressed! That's good.
                    if (!runningLightOn) {
                        debug("turning on the running light");
                        runningLightOn = true;
                        gpioPins->serverOnline(runningLightOn);
                    }
                } else {

                    // Ut oh. We're not progressing.
                    if (runningLightOn) {
                        debug("turning off the running light");
                        runningLightOn = false;
                        gpioPins->serverOnline(runningLightOn);
                    }
                }
                lastFrameSeen = metrics->getTotalFrames();


                // Are we streaming?
                if (lastStreamedFrameSeen < metrics->getFramesStreamed()) {

                    // We're streaming!.
                    if (!streamingLightOn) {
                        debug("turning on the streaming light");
                        streamingLightOn = true;
                        gpioPins->receivingStreamFrames(streamingLightOn);
                    }

                    lastStreamedFrameSeen = metrics->getFramesStreamed();

                } else {

                    // Not streaming!
                    if (streamingLightOn) {
                        debug("turning off the streaming light");
                        streamingLightOn = false;
                        gpioPins->receivingStreamFrames(streamingLightOn);
                    }
                }



                // Are we sending DMX data?
                if (lastDmxEventSeen < metrics->getDMXEventsProcessed()) {

                    // We are!
                    if (!dmxEventLightOn) {
                        debug("turning on the DMX light");
                        dmxEventLightOn = true;
                        gpioPins->sendingDMX(dmxEventLightOn);
                    }

                    lastDmxEventSeen = metrics->getDMXEventsProcessed();

                } else {

                    // Not sending DMX
                    if (dmxEventLightOn) {
                        debug("turning off the DMX light");
                        dmxEventLightOn = false;
                        gpioPins->sendingDMX(dmxEventLightOn);
                    }
                }
            }
        }

        info("StatusLights thread is stopping!");

        // Turn off all the lights on the way out the door, if we can
        if(creatures::gpioPins)
            creatures::gpioPins->allOff();

    }


}