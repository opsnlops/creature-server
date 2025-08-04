

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <oatpp-websocket/WebSocket.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Notice.h"

#include "server/metrics/counters.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/NoticeMessage.h"
#include "server/ws/dto/websocket/WebSocketMessageDto.h"
#include "server/ws/messaging/BasicCommandDto.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/websocket/ClientCafe.h"
#include "server/ws/websocket/ClientConnection.h"

#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
}

namespace creatures ::ws {

void ClientConnection::sendTextMessage(const std::string &message) {
    // appLogger->trace("Sending message to client {}", clientId);
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

    (void)socket;
    (void)message;

    appLogger->trace("pong received from client {}!", clientId);
    metrics->incrementWebsocketPongsReceived();
}

void ClientConnection::onClose(const WebSocket &socket, v_uint16 code, const oatpp::String &message) {

    (void)socket;

    appLogger->debug("onClose code={}, message={}", code, std::string(message));
}

void ClientConnection::readMessage(const WebSocket &socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size) {

    // Silence the warnings about an unused parameter
    (void)opcode;

    if (size == 0) { // message transfer finished

        auto wholeMessage = m_messageBuffer.toString();
        m_messageBuffer.setCurrentPosition(0);

        appLogger->debug("received a message from client {}: {}", clientId, std::string(wholeMessage));

        try {

            // Make a new JSON Mapper that allows for unknown fields
            auto permissiveJsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
            permissiveJsonMapper->getDeserializer()->getConfig()->allowUnknownFields = true;

            // Pull out just the command via a BasicCommandDto.
            auto basicDto = permissiveJsonMapper->readFromString<oatpp::Object<BasicCommandDto>>(wholeMessage);
            if (basicDto) {

                appLogger->trace("request decoded, command: {}", std::string(basicDto->command));

                auto command = basicDto->command;
                messageProcessor->processIncomingMessage(command, wholeMessage);

            } else {
                appLogger->warn("Failed to parse command from message");
            }

        } catch (const oatpp::parser::ParsingError &e) {
            appLogger->warn("parser error decoding a message: {}", e.what());
        } catch (const std::exception &e) {
            appLogger->warn("exception thrown while decoding an incoming message: {}", e.what());
        } catch (...) {

            /*
             * Whoa, the client sent us something we don't know how to handle. Let's let them know.
             */
            appLogger->warn("A websocket client sent us a junk message: {}", std::string(wholeMessage));

            try {
                auto clientMessage =
                    fmt::format("WARNING: You send us a junk message, so we just dropped it on the floor! It was: {}",
                                std::string(wholeMessage));

                // Send a message back to the client to let them know something was wrong
                Notice notice;
                notice.timestamp = getCurrentTimeISO8601();
                notice.message = clientMessage;

                auto message = oatpp::Object<ws::NoticeMessage>::createShared();
                message->command = toString(ws::MessageType::Notice);
                message->payload = creatures::convertToDto(notice);

                // Switch back to the real JSON parser to send things
                std::string messageAsString = apiObjectMapper->writeToString(message);
                socket.sendOneFrameText(messageAsString);
            } catch (...) {
                appLogger->warn("Unable to send a message to a client that sent us junk?!");
            }
        }

        metrics->incrementWebsocketMessagesReceived();

    } else if (size > 0) { // message frame received
        m_messageBuffer.writeSimple(data, size);
    }
}

} // namespace creatures::ws