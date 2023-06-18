
#pragma once

#include <atomic>

/**
 * A helper class to keep track of some counters for system usage
 */
namespace creatures {

    class SystemCounters {

    public:
        SystemCounters();
        ~SystemCounters() = default;

        void incrementTotalFrames();
        void incrementEventsProcessed();
        void incrementFramesStreamed();
        void incrementDMXEventsProcessed();
        void incrementAnimationsPlayed();
        void incrementSoundsPlayed();

        uint64_t getTotalFrames();
        uint64_t getEventsProcessed();
        uint64_t getFramesStreamed();
        uint64_t getDMXEventsProcessed();
        uint64_t getAnimationsPlayed();
        uint64_t getSoundsPlayed();

    private:
        std::atomic<uint64_t> totalFrames;
        std::atomic<uint64_t> eventsProcessed;
        std::atomic<uint64_t> framesStreamed;
        std::atomic<uint64_t> dmxEventsProcessed;
        std::atomic<uint64_t> animationsPlayed;
        std::atomic<uint64_t> soundsPlayed;
    };



}