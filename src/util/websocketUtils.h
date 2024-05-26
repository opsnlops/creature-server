
#pragma once

#include <string>

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

}