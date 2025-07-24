
#include <spdlog/spdlog.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "blockingconcurrentqueue.h"

#include "model/VirtualStatusLights.h"
#include "server/gpio/gpio.h"
#include "server/metrics/StatusLights.h"
#include "server/metrics/counters.h"
#include "util/threadName.h"

#include "server/config.h"
#include "server/namespace-stuffs.h"

#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/VirtualStatusLightsMessage.h"

namespace creatures {

extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<GPIO> gpioPins;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;

StatusLights::StatusLights() {

    lastFrameSeen = 0;
    lastDmxEventSeen = 0;
    lastStreamedFrameSeen = 0;

    runningLightOn = false;
    dmxEventLightOn = false;
    streamingLightOn = false;
    animationLightOn = false;
    soundLightOn = false;

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
    lastAnimationLightOn = animationLightOn;

    while (!stop_requested.load()) {

        bool changesMade = false;

        // This one doesn't need to be as precise as the others. It's okay to stop for
        // the config value. A few missed milliseconds is meaningless here.
        std::this_thread::sleep_for(std::chrono::milliseconds(WATCHDOG_THREAD_PERIOD_MS));

        // Make sure the pointers we touch are valid
        if (metrics != nullptr && gpioPins != nullptr) {

            // Most important one first, let's see if frames are progressing
            if (lastFrameSeen <= metrics->getTotalFrames()) {

                // Time has progressed! That's good.
                if (!runningLightOn) {
                    info("turning on the running light");
                    runningLightOn = true;
                    gpioPins->serverOnline(runningLightOn);
                    changesMade = true;
                }
            } else {

                // Ut oh. We're not progressing.
                if (runningLightOn) {
                    info("turning off the running light");
                    runningLightOn = false;
                    gpioPins->serverOnline(runningLightOn);
                    changesMade = true;
                }
            }
            lastFrameSeen = metrics->getTotalFrames();

            // Are we streaming?
            if (lastStreamedFrameSeen < metrics->getFramesStreamed()) {

                // We're streaming!.
                if (!streamingLightOn) {
                    info("turning on the streaming light");
                    streamingLightOn = true;
                    gpioPins->receivingStreamFrames(streamingLightOn);
                    changesMade = true;
                }

                lastStreamedFrameSeen = metrics->getFramesStreamed();

            } else {

                // Not streaming!
                if (streamingLightOn) {
                    info("turning off the streaming light");
                    streamingLightOn = false;
                    gpioPins->receivingStreamFrames(streamingLightOn);
                    changesMade = true;
                }
            }

            // Are we sending DMX data?
            if (lastDmxEventSeen < metrics->getDMXEventsProcessed()) {

                // We are!
                if (!dmxEventLightOn) {
                    info("turning on the DMX light");
                    dmxEventLightOn = true;
                    gpioPins->sendingDMX(dmxEventLightOn);
                    changesMade = true;
                }

                lastDmxEventSeen = metrics->getDMXEventsProcessed();

            } else {

                // Not sending DMX
                if (dmxEventLightOn) {
                    info("turning off the DMX light");
                    dmxEventLightOn = false;
                    gpioPins->sendingDMX(dmxEventLightOn);
                    changesMade = true;
                }
            }

            // Is there an animation playing?
            if (lastAnimationLightOn != animationLightOn.load()) {
                info("toggling the animation virtual light");
                lastAnimationLightOn = animationLightOn.load();
                changesMade = true;
            }
        }

        if (changesMade) {
            debug("the state of the status lights changed! Sending an update");
            sendUpdateToClients();
        }
    }

    info("StatusLights thread is stopping!");

    // Turn off all the lights on the way out the door, if we can
    if (creatures::gpioPins != nullptr)
        creatures::gpioPins->allOff();
}

void StatusLights::sendUpdateToClients() const {

    // Create the object to send
    auto virtualStatusLights = VirtualStatusLights();
    virtualStatusLights.running = runningLightOn;
    virtualStatusLights.dmx = dmxEventLightOn;
    virtualStatusLights.streaming = streamingLightOn;
    virtualStatusLights.animation_playing = animationLightOn;

    // Create the message to send
    auto message = oatpp::Object<ws::VirtualStatusLightsMessage>::createShared();
    message->command = toString(ws::MessageType::VirtualStatusLights);
    message->payload = convertToDto(virtualStatusLights);

    // Make a JSON mapper
    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

    std::string outgoingMessage = jsonMapper->writeToString(message);
    debug("Outgoing message to clients: {}", outgoingMessage);

    websocketOutgoingMessages->enqueue(outgoingMessage);
}

} // namespace creatures