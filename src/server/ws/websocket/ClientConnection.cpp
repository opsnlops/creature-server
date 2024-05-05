
#include <atomic>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp-websocket/ConnectionHandler.hpp>
#include <oatpp-websocket/WebSocket.hpp>

#include <oatpp/core/macro/component.hpp>

#include "server/metrics/counters.h"
#include "server/ws/websocket/ClientConnection.h"
#include "server/ws/websocket/ClientCafe.h"

namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}


namespace creatures :: ws {

    void ClientConnection::sendTextMessage(const WebsocketMessage& message) {
        appLogger->debug("Sending message to client {}", clientId);

        OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper);

        auto dto = convertToDto(message);
        auto messageToSend = apiObjectMapper->writeToString(dto);

        ourSocket.sendOneFrameText(messageToSend);
        creatures::metrics->incrementWebsocketMessagesSent();
    }

    void ClientConnection::sendPing() {
        appLogger->debug("Sending ping to client {}", clientId);
        ourSocket.sendPing("ping"); // We should fo reference counting here
    }

    void ClientConnection::onPing(const WebSocket& socket, const oatpp::String& message) {
        appLogger->debug("onPing");
        socket.sendPong(message);
    }

    void ClientConnection::onPong(const WebSocket& socket, const oatpp::String& message) {
        appLogger->trace("pong received from client {}!", clientId);
        metrics->incrementWebsocketPongsReceived();
    }

    void ClientConnection::onClose(const WebSocket& socket, v_uint16 code, const oatpp::String& message) {
        appLogger->debug("onClose code={}", code);
    }

    void ClientConnection::readMessage(const WebSocket& socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size) {


        if(size == 0) { // message transfer finished

            auto wholeMessage = m_messageBuffer.toString();
            m_messageBuffer.setCurrentPosition(0);

            appLogger->debug("received a message from client {}: {}", clientId, std::string(wholeMessage));

            /* Send message in reply */
            std::string message = fmt::format("Hello from client {}!: {}", clientId, std::string(wholeMessage));
            socket.sendOneFrameText(message);

            metrics->incrementWebsocketMessagesReceived();

        } else if(size > 0) { // message frame received
            m_messageBuffer.writeSimple(data, size);
        }

    }

}