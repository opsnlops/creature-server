
#pragma once

#include <atomic>
#include <mutex>
#include <vector>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp-websocket/ConnectionHandler.hpp>
#include <oatpp-websocket/WebSocket.hpp>

#include <oatpp/core/macro/component.hpp>

#include "server/ws/websocket/ClientCafe.h"
#include "server/ws/websocket/ClientConnection.h"

namespace creatures :: ws {

    class ClientCafe; // Forward declaration

    /**
     * One connection to a client!
     */
class ClientConnection : public oatpp::websocket::WebSocket::Listener {


    public:

        ClientConnection(const oatpp::websocket::WebSocket& socket, v_int64 clientId, std::shared_ptr<ClientCafe> cafe)
                : clientId(clientId), ourSocket(socket), cafe(cafe) {
            appLogger->debug("Client {} checking in!", clientId);
        }
        /**
         * Called on "ping" frame.
         */
        void onPing(const WebSocket& socket, const oatpp::String& message) override;

        /**
         * Called on "pong" frame
         */
        void onPong(const WebSocket& socket, const oatpp::String& message) override;

        /**
         * Called on "close" frame
         */
        void onClose(const WebSocket& socket, v_uint16 code, const oatpp::String& message) override;

        /**
         * Called on each message frame. After the last message will be called once-again with size == 0 to designate end of the message.
         */
        void readMessage(const WebSocket& socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size) override;

        /**
         * Send a message to our client
         *
         * @param message the message to send
         */
        void sendTextMessage(const std::string& message);

        /**
         * Our client ID
         */
        v_int64 clientId;


        /**
         * Send a ping to the client
         */
        void sendPing();


    private:
        /**
         * Buffer for messages. Needed for multi-frame messages.
         */
        oatpp::data::stream::BufferOutputStream m_messageBuffer;

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
        OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper);
        OATPP_COMPONENT(std::shared_ptr<creatures::ws::MessageProcessor>, messageProcessor);

        /**
         * The socket we are connected to
         */
        const oatpp::websocket::WebSocket& ourSocket;

        std::shared_ptr<ClientCafe> cafe;

    };

}