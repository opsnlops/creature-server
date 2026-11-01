#pragma once

namespace creatures::transport {

/**
 * Process-owned HTTP/WebSocket transport lifecycle.
 *
 * A transport is selected once at startup. shutdown() must stop admission and
 * join every transport-owned thread before returning so the creature event
 * loop and shared services can be destroyed safely afterward.
 */
class TransportServer {
  public:
    virtual ~TransportServer() = default;

    virtual void start() = 0;
    virtual void shutdown() = 0;
};

} // namespace creatures::transport
