
#include <vector>
#include <sstream>
#include <iostream>

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/dmx/dmx.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "util/cache.h"

#include "server/namespace-stuffs.h"

namespace creatures {

    extern std::shared_ptr<ObjectCache<std::string, DMX>> dmxCache;
    extern std::shared_ptr<SystemCounters> metrics;

    void DMXEvent::executeImpl() {

        std::shared_ptr<DMX> sender;

#if DEBUG_EVENT_DMX
        trace("doing a DMX send event");
#endif
        // Create the key
        std::string key = clientIP + "-" + (use_multicast ? "multicast" : "unicast") + "-" +  std::to_string(dmxUniverse) + "-" + std::to_string(dmxOffset) + "-" + std::to_string(numMotors);

#if DEBUG_EVENT_DMX
        trace("key: {}", key);
#endif

        // See if there's an existing DMX in the cache
        try {
            sender = dmxCache->get(key);
        } catch(const std::out_of_range &e) {

            debug("DMX sender not found in cache, making one now");
            sender = std::make_shared<DMX>();
            sender->init(clientIP, use_multicast, dmxUniverse, numMotors);

            info("put new sender for {} in the cache", key);
            dmxCache->put(key, sender);
        }

        // Now send the packet
        sender->send(data);

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