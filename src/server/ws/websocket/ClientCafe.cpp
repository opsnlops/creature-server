
#include <chrono>
#include <mutex>
#include <vector>
#include <spdlog/spdlog.h>

#include <oatpp/core/macro/component.hpp>

#include "model/WebsocketMessage.h"

#include "server/metrics/counters.h"
#include "server/ws/websocket/ClientConnection.h"
#include "server/ws/websocket/ClientCafe.h"

#include "util/MessageQueue.h"
#include "util/threadName.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<MessageQueue<WebsocketMessage>> websocketOutgoingMessages;
}

namespace creatures :: ws {


    std::atomic<v_int32> ClientCafe::clientsConnected(0);


    void ClientCafe::broadcastMessage(const WebsocketMessage& message) {

        OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper);
        appLogger->debug("Broadcasting message to all clients");

        std::lock_guard<std::mutex> guard(clientConnectionMapMutex);
        for (const auto &client: clientConnectionMap) {
            client.second->sendTextMessage(message);
        }
    }


    v_int64 ClientCafe::getNextClientId() {
        return clientIdCounter++;
    }

    void ClientCafe::onAfterCreate(const oatpp::websocket::WebSocket& socket, const std::shared_ptr<const ParameterMap>& params) {


        clientsConnected ++;
        appLogger->debug("New client connection! Total connected: {}", clientsConnected.load());

        v_int64 clientId = getNextClientId();
        auto client =  std::make_shared<ClientConnection>(socket, clientId, shared_from_this());

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


    void ClientCafe::onBeforeDestroy(const oatpp::websocket::WebSocket& socket) {

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

    [[noreturn]] void ClientCafe::runMessageLoop() {

        // Make sure this shows up in the debugger correctly
        setThreadName("ClientCafe::runMessageLoop");

        while(true) {
            WebsocketMessage message = creatures::websocketOutgoingMessages->pop();
            broadcastMessage(message);
            appLogger->debug("Sent message to all clients from the queue");
        }
    }

    [[noreturn]] void ClientCafe::runPingLoop(const std::chrono::duration<v_int64, std::micro>& interval) {

        // Make sure this shows up in the debugger correctly
        setThreadName("ClientCafe::runPingLoop");

        while(true) {

            std::chrono::duration<v_int64, std::micro> elapsed = std::chrono::microseconds(0);
            auto startTime = std::chrono::system_clock::now();

            appLogger->debug("Running ping loop");

            do {
                std::this_thread::sleep_for(interval - elapsed);
                elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now() - startTime);
            } while (elapsed < interval);

            // Keep the scope tight on this lock
            {
                std::lock_guard<std::mutex> lock(clientConnectionMapMutex);
                for (const auto &client: clientConnectionMap) {
                    client.second->sendPing();
                    metrics->incrementWebsocketPingsSent();
                }
            }

        }

    }

}