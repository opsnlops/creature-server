
#include "spdlog/spdlog.h"

#include "util/helpers.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    PlaylistEvent::PlaylistEvent(uint64_t frameNumber, CreatureId creatureId)
            : EventBase(frameNumber), creatureId(std::move(creatureId)) {}

    void PlaylistEvent::executeImpl() {


        debug("hello from a playlist event for creature {}", creatureIdToString(creatureId));


        // Make another event
        //auto nextTick = std::make_shared<TickEvent>(this->frameNumber + TICK_TIME_FRAMES);
        //eventLoop->scheduleEvent(nextTick);

        metrics->incrementPlaylistsEventsProcessed();
    }

}