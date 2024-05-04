
#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    void TickEvent::executeImpl() {

        // Just go tick!
        debug("⌚️ Hello from frame {:L}! Event queue length: {}, events: {:L}, frames streamed: {:L}, animations played: {:L}, DMX events sent: {:L}, sounds played: {:L}, playlists started: {:L}, playlists stopped: {:L}, playlists events processed: {:L}, web api requests: {:L}",
              eventLoop->getCurrentFrameNumber(),
              eventLoop->getQueueSize(),
              metrics->getEventsProcessed(),
              metrics->getFramesStreamed(),
              metrics->getAnimationsPlayed(),
              metrics->getDMXEventsProcessed(),
              metrics->getSoundsPlayed(),
              metrics->getPlaylistsStarted(),
              metrics->getPlaylistsStopped(),
              metrics->getPlaylistsEventsProcessed(),
              metrics->getRestRequestsProcessed());


        // Make another event
        auto nextTick = std::make_shared<TickEvent>(this->frameNumber + TICK_TIME_FRAMES);
        eventLoop->scheduleEvent(nextTick);
    }

}