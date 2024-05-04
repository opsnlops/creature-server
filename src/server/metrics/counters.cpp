
#include "counters.h"


namespace creatures {

    SystemCounters::SystemCounters() {
        totalFrames = 0;
        eventsProcessed = 0;
        framesStreamed = 0;
        dmxEventsProcessed = 0;
        animationsPlayed = 0;
        soundsPlayed = 0;
        playlistsStarted = 0;
        playlistsStopped = 0;
        playlistsEventsProcessed = 0;
        playlistStatusRequests = 0;
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

    void SystemCounters::incrementPlaylistsStarted() {
        playlistsStarted++;
    }

    void SystemCounters::incrementPlaylistsStopped() {
        playlistsStopped++;
    }

    void SystemCounters::incrementPlaylistsEventsProcessed() {
        playlistsEventsProcessed++;
    }

    void SystemCounters::incrementPlaylistStatusRequests() {
        playlistStatusRequests++;
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

    uint64_t SystemCounters::getPlaylistsStarted() {
        return playlistsStarted.load();
    }

    uint64_t SystemCounters::getPlaylistsStopped() {
        return playlistsStopped.load();
    }

    uint64_t SystemCounters::getPlaylistsEventsProcessed() {
        return playlistsEventsProcessed.load();
    }

    uint64_t SystemCounters::getPlaylistStatusRequests() {
        return playlistStatusRequests.load();
    }

    /**
     * Create a DTO from the current state of the counters
     *
     * @return a shared pointer to the DTO
     */
    std::shared_ptr<SystemCountersDto> SystemCounters::convertToDto() {

        auto dto = SystemCountersDto::createShared();

        dto->totalFrames = totalFrames.load();
        dto->eventsProcessed = eventsProcessed.load();
        dto->framesStreamed = framesStreamed.load();
        dto->dmxEventsProcessed = dmxEventsProcessed.load();
        dto->animationsPlayed = animationsPlayed.load();
        dto->soundsPlayed = soundsPlayed.load();
        dto->playlistsStarted = playlistsStarted.load();
        dto->playlistsStopped = playlistsStopped.load();
        dto->playlistsEventsProcessed = playlistsEventsProcessed.load();
        dto->playlistStatusRequests = playlistStatusRequests.load();

        return dto.getPtr();

    }
}
