#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

#include "server/transport/TransportServer.h"

namespace creatures::transport {

/**
 * Default production uWebSockets transport during the oat++ rollback window.
 *
 * The uWebSockets app and every live socket remain on thread_. Cross-thread
 * shutdown enters that ownership domain only through Loop::defer.
 */
class UWebSocketsServer final : public TransportServer {
  public:
    UWebSocketsServer(uint32_t maximumConnections, uint32_t maximumConnectionsPerPeer);
    ~UWebSocketsServer() override;

    UWebSocketsServer(const UWebSocketsServer &) = delete;
    UWebSocketsServer &operator=(const UWebSocketsServer &) = delete;

    void start() override;
    void shutdown() override;

    [[nodiscard]] uint16_t boundPort() const override;

  private:
    enum class StartState {
        NotStarted,
        Starting,
        Running,
        Failed,
        Stopped,
    };

    void run();
    void publishStartState(StartState state, std::string error = {});

    mutable std::mutex lifecycleMutex_; // mutable: boundPort() is const but reads under the lock
    std::condition_variable lifecycleCondition_;
    std::thread thread_;
    StartState startState_{StartState::NotStarted};
    std::string startError_;
    void *loop_{nullptr};
    void *app_{nullptr};
    std::function<void()> shutdownAction_;
    bool shutdownRequested_{false};
    uint16_t boundPort_{0};
    uint32_t maximumConnections_;
    uint32_t maximumConnectionsPerPeer_;
};

} // namespace creatures::transport
