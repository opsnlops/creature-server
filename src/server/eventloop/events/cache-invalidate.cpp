
#include <future>
#include <spdlog/spdlog.h>

#include "model/CacheInvalidation.h"
#include "server/eventloop/events/types.h"
#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"

namespace creatures {


    CacheInvalidateEvent::CacheInvalidateEvent(framenum_t frameNumber, CacheType _cacheType)
            : EventBase(frameNumber), cacheType(_cacheType) {}

    /**
     * Tells the clients to invalidate a certain cache type
     */
    void CacheInvalidateEvent::executeImpl() {
        debug("cache invalidate event for {}", toString(cacheType));
        broadcastCacheInvalidationToAllClients(cacheType);
    }

}