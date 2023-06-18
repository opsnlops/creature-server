
#include "counters.h"


namespace creatures {

    SystemCounters::SystemCounters() {
        totalFrames = 0;
        eventsProcessed = 0;
        framesStreamed = 0;
        dmxEventsProcessed = 0;
        animationsPlayed = 0;
        soundsPlayed = 0;
    }

    void SystemCounters::incrementTotalFrames() {
        totalFrames++;
    }

    void SystemCounters::incrementEventsProcessed() {
        eventsProcessed++;
    }

    void SystemCounters::incrementFramesStreamed() {
        framesStreamed++;
    }

    void SystemCounters::incrementDMXEventsProcessed() {
        dmxEventsProcessed++;
    }

    void SystemCounters::incrementAnimationsPlayed() {
        animationsPlayed++;
    }

    void SystemCounters::incrementSoundsPlayed() {
        soundsPlayed++;
    }

    uint64_t SystemCounters::getTotalFrames() {
        return totalFrames.load();
    }

    uint64_t SystemCounters::getEventsProcessed() {
        return eventsProcessed.load();
    }

    uint64_t SystemCounters::getFramesStreamed() {
        return framesStreamed.load();
    }

    uint64_t SystemCounters::getDMXEventsProcessed() {
        return dmxEventsProcessed.load();
    }

    uint64_t SystemCounters::getAnimationsPlayed() {
        return animationsPlayed.load();
    }

    uint64_t SystemCounters::getSoundsPlayed() {
        return soundsPlayed.load();
    }
}
