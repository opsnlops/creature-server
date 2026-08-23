

#include <spdlog/spdlog.h>

#include <oatpp-websocket/WebSocket.hpp>
#include <oatpp/core/macro/component.hpp>

#include "api/WebSocketEnvelope.h"
#include "model/Notice.h"

#include "server/metrics/counters.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/messaging/MessageProcessor.h"
#include "server/ws/websocket/ClientCafe.h"
#include "server/ws/websocket/ClientConnection.h"

#include "util/JsonParser.h"
#include "util/helpers.h"

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
}

namespace creatures ::ws {

void ClientConnection::sendTextMessage(const std::string &message) {
    // appLogger->trace("Sending message to client {}", clientId);
    try {
        ourSocket.sendOneFrameText(message);
        creatures::metrics->incrementWebsocketMessagesSent();
    } catch (const std::runtime_error &e) {
        appLogger->warn("Failed to send message to client {}: {} (likely disconnected)", clientId, e.what());
    } catch (const std::exception &e) {
        appLogger->warn("Exception sending message to client {}: {}", clientId, e.what());
    } catch (...) {
        appLogger->warn("Unknown exception sending message to client {}", clientId);
    }
}

void ClientConnection::sendPing() {
    appLogger->debug("Sending ping to client {}", clientId);
    try {
        ourSocket.sendPing("ping"); // We should be reference counting here
    } catch (const std::runtime_error &e) {
        appLogger->warn("Failed to send ping to client {}: {} (likely disconnected)", clientId, e.what());
    } catch (const std::exception &e) {
        appLogger->warn("Exception sending ping to client {}: {}", clientId, e.what());
    } catch (...) {
        appLogger->warn("Unknown exception sending ping to client {}", clientId);
    }
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

// Cap accumulated inbound message size to bound memory under a hostile
// client that sends an unbounded multi-frame message. If a client tries
// to exceed this we drop the buffer and close the connection — the JSON
// payloads we actually use are well under 64 KiB.
static constexpr oatpp::v_io_size MAX_INBOUND_MESSAGE_BYTES = 64ULL * 1024;

void ClientConnection::readMessage(const WebSocket &socket, v_uint8 opcode, p_char8 data, oatpp::v_io_size size) {

    // Silence the warnings about an unused parameter
    (void)opcode;
    static_cast<void>(socket);

    if (m_bufferOverflowed) {
        // We've already decided to drop this message. Keep ignoring frames
        // until the terminator (size == 0) and then reset.
        if (size == 0) {
            m_messageBuffer.setCurrentPosition(0);
            m_bufferOverflowed = false;
            appLogger->warn("Dropped oversized message from client {} (cap={} bytes)", clientId,
                            MAX_INBOUND_MESSAGE_BYTES);
        }
        return;
    }

    if (size == 0) { // message transfer finished

        auto wholeMessage = m_messageBuffer.toString();
        m_messageBuffer.setCurrentPosition(0);

        appLogger->debug("received a {} byte message from client {}", wholeMessage->size(), clientId);

        try {
            const auto parsedMessage = JsonParser::parseApiJsonString(std::string(wholeMessage), "WebSocket message");
            if (parsedMessage.isSuccess()) {
                messageProcessor->processIncomingMessage(parsedMessage.getValue().value(), wholeMessage);
            } else {
                appLogger->warn("Rejected inbound WebSocket message: {}", parsedMessage.getError()->getMessage());
                const Notice notice{getCurrentTimeISO8601(), "Dropped malformed WebSocket message."};
                sendTextMessage(
                    api::serializeWebSocketEnvelope(toString(ws::MessageType::Notice), noticeToJson(notice)));
            }
        } catch (const std::exception &error) {
            appLogger->warn("Exception while processing inbound WebSocket message from client {}: {}", clientId,
                            error.what());
        } catch (...) {
            appLogger->warn("Unknown exception while processing inbound WebSocket message from client {}", clientId);
        }

        metrics->incrementWebsocketMessagesReceived();

    } else if (size > 0) { // message frame received
        if (m_messageBuffer.getCurrentPosition() + size > MAX_INBOUND_MESSAGE_BYTES) {
            // Will exceed the cap. Drop everything from this message and
            // mark overflow so we ignore subsequent frames until size==0.
            m_bufferOverflowed = true;
            m_messageBuffer.setCurrentPosition(0);
            return;
        }
        m_messageBuffer.writeSimple(data, size);
    }
}

} // namespace creatures::ws
