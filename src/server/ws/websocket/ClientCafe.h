
#pragma once


#include <atomic>
#include <mutex>
#include <vector>


#include <oatpp-websocket/ConnectionHandler.hpp>
#include <oatpp-websocket/WebSocket.hpp>

#include <oatpp/core/macro/component.hpp>

#include "server/ws/websocket/ClientConnection.h"

namespace creatures :: ws {


    class ClientConnection; // Forward declaration

    /**
     * This is where the client connections hang out
     *
     * It's just a place for them to hang out for and me to keep track of who is currently connected.
     */
    class ClientCafe : public oatpp::websocket::ConnectionHandler::SocketInstanceListener,
                       public std::enable_shared_from_this<ClientCafe> {

    public:

        ClientCafe() : clientIdCounter(0) {}
        virtual ~ClientCafe() {};

        /**
         * Broadcast a message to all connected clients
         */
        void broadcastMessage(const std::string& message);


        /**
         * Gets the next client ID
         * @return the next available client ID
         */
        v_int64 getNextClientId();

        /**
         *  Called when socket is created
         */
        void onAfterCreate(const oatpp::websocket::WebSocket& socket, const std::shared_ptr<const ParameterMap>& params) override;

        /**
         *  Called before socket instance is destroyed.
         */
        void onBeforeDestroy(const oatpp::websocket::WebSocket& socket) override;

        /**
         * Run the ping loop
         *
         * @param interval how long to wait between pings (default 30 seconds)
         */
        [[noreturn]] void runPingLoop(const std::chrono::duration<v_int64, std::micro>& interval = std::chrono::seconds(30));

        /**
         * Run the message loop
         */
        [[noreturn]] void runMessageLoop();

    private:

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        /**
         * Counter for connected clients
         */
        static std::atomic<v_int32> clientsConnected;

        /**
         * Counter for client IDs
         */
        std::atomic<v_int64> clientIdCounter;

        /**
         * Map of client connections (and a mutex to protect it)
         */
        std::unordered_map<v_int64, std::shared_ptr<ClientConnection>> clientConnectionMap;
        std::mutex clientConnectionMapMutex;
    };






}