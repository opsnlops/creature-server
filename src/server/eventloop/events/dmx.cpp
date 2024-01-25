
#include <vector>
#include <sstream>
#include <iostream>

#include "spdlog/spdlog.h"

#include <E131Server.h>

#include "server/config.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "util/cache.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr <creatures::e131::E131Server> e131Server;
    extern std::shared_ptr<SystemCounters> metrics;

    void DMXEvent::executeImpl() {

        // Send the DMX data
        e131Server->setValues(dmxUniverse, dmxOffset, data);

        // Update our metrics
        metrics->incrementDMXEventsProcessed();

#if DEBUG_EVENT_DMX

        std::ostringstream oss;
        for (const auto& value : data) {
            oss << static_cast<int>(value) << " ";
        }
        trace("Frame data: {}", oss.str());

        debug(" -------> DMX data sent");
#endif

    }

}