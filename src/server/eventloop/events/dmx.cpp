
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

#if DEBUG_EVENT_DMX
        trace("doing a DMX send event");
#endif

        std::shared_ptr<DMX> sender;

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
            sender = std::make_unique<DMX>();
            sender->init(clientIP, dmxUniverse, numMotors);

            info("put sender for {} in the cache", key);
            dmxCache->put(key, sender);
        }

        // Now send the packet
        sender->send(data);
#if DEBUG_EVENT_DMX
        debug(" -------> DMX data sent");
#endif

    }

}