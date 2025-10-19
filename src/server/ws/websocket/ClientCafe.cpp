
#include <mutex>

#include <spdlog/spdlog.h>

#include <oatpp/core/macro/component.hpp>

#include "blockingconcurrentqueue.h"

#include "server/metrics/counters.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/websocket/ClientCafe.h"
#include "server/ws/websocket/ClientConnection.h"

#include "util/threadName.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
} // namespace creatures

#define DEBUG_WS_LOGGING 0

namespace creatures ::ws {

std::atomic<v_int32> ClientCafe::clientsConnected(0);

void ClientCafe::broadcastMessage(const std::string &message) {

#if DEBUG_WS_LOGGING
    appLogger->debug("Broadcasting message to all clients");
#endif

    std::lock_guard<std::mutex> guard(clientConnectionMapMutex);
    for (const auto &client : clientConnectionMap) {
        client.second->sendTextMessage(message);
    }
}

v_int64 ClientCafe::getNextClientId() { return clientIdCounter++; }

void ClientCafe::onAfterCreate(const oatpp::websocket::WebSocket &socket,
                               const std::shared_ptr<const ParameterMap> &params) {

    (void)params;

    clientsConnected++;
    appLogger->debug("New client connection! Total connected: {}", clientsConnected.load());

    v_int64 clientId = getNextClientId();
    auto client = std::make_shared<ClientConnection>(socket, clientId, shared_from_this());

    // Add the client to the map
    {
        std::lock_guard<std::mutex> guard(clientConnectionMapMutex);
        clientConnectionMap[clientId] = client;
    }

    // Create a new client connection for this socket
    socket.setListener(client);

    // Update our counters
    creatures::metrics->incrementWebsocketConnectionsProcessed();
}

void ClientCafe::onBeforeDestroy(const oatpp::websocket::WebSocket &socket) {

    clientsConnected--;
    appLogger->debug("Client saying goodbye! New client count: {}", clientsConnected.load());

    // Get the client connection and remove it
    auto client = std::static_pointer_cast<ClientConnection>(socket.getListener());
    appLogger->info("Client {} disconnected 👋🏻", client->clientId);

    // Remove the client from the map
    {
        std::lock_guard<std::mutex> guard(clientConnectionMapMutex);
        clientConnectionMap.erase(client->clientId);
    }
}

void ClientCafe::runMessageLoop() {

    // Make sure this shows up in the debugger correctly
    setThreadName("ClientCafe::runMessageLoop");

    std::string message;
    while (!shutdownRequested.load()) {
        // Use wait_dequeue_timed instead of wait_dequeue to allow checking shutdown flag
        if (creatures::websocketOutgoingMessages->wait_dequeue_timed(message, std::chrono::milliseconds(100))) {
            if (!shutdownRequested.load()) {
                broadcastMessage(message);
            }
        }
    }
    appLogger->info("Message loop exiting gracefully");
}

void ClientCafe::runPingLoop(const std::chrono::duration<v_int64, std::micro> &interval) {

    // Make sure this shows up in the debugger correctly
    setThreadName("ClientCafe::runPingLoop");

    appLogger->info("Starting the ping loop");

    while (!shutdownRequested.load()) {

        std::chrono::duration<v_int64, std::micro> elapsed = std::chrono::microseconds(0);
        auto startTime = std::chrono::system_clock::now();

        appLogger->debug("pinging all websocket clients");

        // Sleep in smaller chunks to allow checking shutdown flag
        do {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - startTime);
            if (shutdownRequested.load()) {
                break;
            }
        } while (elapsed < interval);

        // Only send pings if not shutting down
        if (!shutdownRequested.load()) {
            // Keep the scope tight on this lock
            std::lock_guard<std::mutex> lock(clientConnectionMapMutex);
            for (const auto &client : clientConnectionMap) {
                client.second->sendPing();
                metrics->incrementWebsocketPingsSent();
            }
        }
    }
    appLogger->info("Ping loop exiting gracefully");
}

void ClientCafe::requestShutdown() {
    appLogger->info("Shutdown requested for ClientCafe");
    shutdownRequested.store(true);
}

} // namespace creatures::ws