
#include "spdlog/spdlog.h"

#include "server/gpio/gpio.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/event.h"
#include "server/metrics/StatusLights.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<GPIO> gpioPins;
    extern std::shared_ptr<StatusLights> statusLights;

    /**
     * Turns one of the status lights on or off via an event
     *
     * @param frameNumber frame number to schedule this event for
     * @param light An enum from StatusLight
     * @param on Should the light be on?
     */
    StatusLightEvent::StatusLightEvent(framenum_t frameNumber, StatusLight light, bool on)
            : EventBase(frameNumber), light(light), on(on) {}

    void StatusLightEvent::executeImpl() {

        std::string lightName;

        switch(light) {
            case StatusLight::Animation:
                gpioPins->playingAnimation(on);
                statusLights->animationLightOn = on;
                lightName = "Animation";
                break;

            default:
                warn("Unhandled light in StatusLightEvent: {}", static_cast<int>(light));
                return;

        }

        debug("Turning status light {} {}", lightName, on ? "on" : "off");

    }

}