
#include <vector>
#include <sstream>
#include <iostream>

#include "spdlog/spdlog.h"

#include "server/config.h"
#include "server/dmx/dmx.h"
#include "server/eventloop/events/types.h"
#include "util/cache.h"

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;


namespace creatures {

    extern std::shared_ptr<ObjectCache<std::string, DMX>> dmxCache;

    void DMXEvent::executeImpl() {

        std::shared_ptr<DMX> sender;

#if DEBUG_EVENT_DMX
        trace("doing a DMX send event");
#endif
        // Create the key
        std::string key = clientIP + "-" + std::to_string(dmxUniverse) + "-" + std::to_string(dmxOffset) + "-" + std::to_string(numMotors);

#if DEBUG_EVENT_DMX
        trace("key: {}", key);
#endif

        // See if there's an existing DMX in the cache
        try {
            sender = dmxCache->get(key);
        } catch(const std::out_of_range &e) {

            debug("DMX sender not found in cache, making one now");
            sender = std::make_shared<DMX>();
            sender->init(clientIP, dmxUniverse, numMotors);

            info("put new sender for {} in the cache", key);
            dmxCache->put(key, sender);
        }



        // Now send the packet
        sender->send(data);

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