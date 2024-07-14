
#pragma once

#include <string>

#include "model/CacheInvalidation.h"
#include "util/Result.h"

#include "server/namespace-stuffs.h"


namespace creatures {

    /**
     * Broadcast out a message to all clients that are currently connected
     *
     * @param message the message to send
     * @return true is the message was sent, ServerError if there was an issue
     */
    Result<bool> broadcastNoticeToAllClients(const std::string &message);

    /**
     * Broadcast a message to any client listening that they should invalidate a certain
     * type of cache
     *
     * @param type the `CacheType` that should be invalidated
     * @return true if the message was sent, ServerError if there was an issue
     */
    Result<bool> broadcastCacheInvalidationToAllClients(const CacheType &type);


    /**
     * Schedule a cache invalidation event for some time in the future.
     *
     * This is useful to help prevent data races on the clients. I don't want to send a cache invalidate
     * message _before_ the client is done thinking that we're updating something on our side. This allows
     * for a delay to occur, leveraging our existing event loop.
     *
     * @param frameOffset how many frames from now should this invalidation be scheduled?
     * @param type which type?
     */
    void scheduleCacheInvalidationEvent(framenum_t frameOffset, CacheType type);

}