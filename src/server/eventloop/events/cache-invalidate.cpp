
#include <future>
#include <spdlog/spdlog.h>

#include "model/CacheInvalidation.h"
#include "server/eventloop/events/types.h"
#include "server/namespace-stuffs.h"
#include "util/websocketUtils.h"

extern std::shared_ptr<creatures::EventLoop> eventLoop;

namespace creatures {


    CacheInvalidateEvent::CacheInvalidateEvent(framenum_t frameNumber, CacheType _cacheType)
            : EventBase(frameNumber), cacheType(_cacheType) {}

    /**
     * Tells the clients to invalidate a certain cache type
     */
    Result<framenum_t> CacheInvalidateEvent::executeImpl() {
        debug("cache invalidate event for {}", toString(cacheType));
        broadcastCacheInvalidationToAllClients(cacheType);

        return Result{this->frameNumber};
    }


}