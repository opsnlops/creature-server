
#include <atomic>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp-websocket/ConnectionHandler.hpp>
#include <oatpp-websocket/WebSocket.hpp>

#include <oatpp/core/macro/component.hpp>


#include "model/Notice.h"

#include "server/metrics/counters.h"
#include "server/ws/websocket/ClientConnection.h"
#include "server/ws/websocket/ClientCafe.h"
#include "server/ws/dto/websocket/NoticeMessage.h"
#include "server/ws/dto/websocket/MessageTypes.h"

#include "util/helpers.h"


namespace creatures {
    extern std::shared_ptr<SystemCounters> metrics;
}


namespace creatures ::ws {

    void ClientConnection::sendTextMessage(const std::string &message) {
        //appLogger->trace("Sending message to client {}", clientId);
        ourSocket.sendOneFrameText(message);
        creatures::metrics->incrementWebsocketMessagesSent();
    }

    void ClientConnection::sendPing() {
        appLogger->debug("Sending ping to client {}", clientId);
        ourSocket.sendPing("ping"); // We should be reference counting here
    }

    void ClientConnection::onPing(const WebSocket &socket, const oatpp::String &message) {
        appLogger->debug("client {} sent a ping!", clientId);
        socket.sendPong(message);
    }

    void ClientConnection::onPong(const WebSocket &socket, const oatpp::String &message) {
        appLogger->trace("pong received from client {}!", clientId);
        metrics->incrementWebsocketPongsReceived();
    }

    void ClientConnection::onClose(const WebSocket &socket, v_uint16 code, const oatpp::String &message) {
        appLogger->debug("onClose code={}, message={}", code, std::string(message));
    }

    void ClientConnection::readMessage(const WebSocket &socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size) {

        // Silence the warnings about an unused parameter
        (void) opcode;

        if (size == 0) { // message transfer finished

            auto wholeMessage = m_messageBuffer.toString();
            m_messageBuffer.setCurrentPosition(0);

            appLogger->debug("received a message from client {}: {}", clientId, std::string(wholeMessage));

            std::string clientMessage = fmt::format("Hello from client {}!: {}", clientId, std::string(wholeMessage));


            Notice notice;
            notice.timestamp = getCurrentTimeISO8601();
            notice.message = clientMessage;


            auto message = oatpp::Object<ws::NoticeMessage>::createShared();
            message->command = toString(ws::MessageType::Notice);
            message->payload = creatures::convertToDto(notice);


            std::string messageAsString = apiObjectMapper->writeToString(message);

            socket.sendOneFrameText(messageAsString);
            metrics->incrementWebsocketMessagesReceived();

        } else if (size > 0) { // message frame received
            m_messageBuffer.writeSimple(data, size);
        }

    }

}