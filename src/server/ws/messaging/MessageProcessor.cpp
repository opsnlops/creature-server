

#include <spdlog/spdlog.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>

#include "api/WebSocketEnvelope.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "util/ObservabilityManager.h"

#include "DynamixelSensorReportHandler.h"
#include "MessageProcessor.h"
#include "NoticeMessageHandler.h"
#include "SensorReportHandler.h"
#include "StreamFrameHandler.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::ws {

MessageProcessor::MessageProcessor() {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    appLogger->info("Creating the MessageProcessor");

    // Register the handles
    handlers[toString(MessageType::Notice)] = std::make_unique<creatures::ws::NoticeMessageHandler>();
    appLogger->debug("added the handler for {}", toString(MessageType::Notice));

    handlers[toString(MessageType::StreamFrame)] = std::make_unique<creatures::ws::StreamFrameHandler>();
    appLogger->debug("added the handler for {}", toString(MessageType::StreamFrame));

    handlers[toString(MessageType::BoardSensorReport)] = std::make_unique<creatures::ws::SensorReportHandler>();
    appLogger->debug("added the handler for {}", toString(MessageType::BoardSensorReport));

    handlers[toString(MessageType::MotorSensorReport)] = std::make_unique<creatures::ws::SensorReportHandler>();
    appLogger->debug("added the handler for {}", toString(MessageType::MotorSensorReport));

    handlers[toString(MessageType::DynamixelSensorReport)] =
        std::make_unique<creatures::ws::DynamixelSensorReportHandler>();
    appLogger->debug("added the handler for {}", toString(MessageType::DynamixelSensorReport));

    // Log how many we have total
    appLogger->info("{} message handler{} registered", handlers.size(), handlers.size() != 1 ? "s" : "");
}

void MessageProcessor::processIncomingMessage(const nlohmann::json &envelope, const oatpp::String &message) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    auto span = observability ? observability->createSamplingSpan("WebSocket.inbound", 0.0005) : nullptr;
    if (span) {
        span->setAttribute("websocket.message.size", static_cast<int64_t>(message ? message->size() : 0));
    }

    const auto parsedEnvelope = api::webSocketEnvelopeFromJson(envelope);
    if (!parsedEnvelope.isSuccess()) {
        const auto errorMessage = parsedEnvelope.getError()->getMessage();
        appLogger->warn("Rejected inbound WebSocket envelope: {}", errorMessage);
        if (span) {
            span->setError(errorMessage);
            span->setAttribute("websocket.message.size", static_cast<int64_t>(message ? message->size() : 0));
            span->setAttribute("websocket.envelope.valid", false);
            span->setAttribute("websocket.rejection.stage", "envelope");
            span->setAttribute("websocket.error.type", "InvalidEnvelope");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return;
    }
    const auto parsedValue = parsedEnvelope.getValue();
    const auto &parsed = parsedValue.value();
    if (span) {
        span->setAttribute("websocket.command", parsed.command);
        span->setAttribute("websocket.envelope.valid", true);
    }

    // Find the right handler
    if (const auto handler = handlers.find(parsed.command); handler != handlers.end()) {
        if (span) {
            span->setAttribute("websocket.handler", parsed.command);
        }
        const bool accepted = handler->second->processMessage(parsed.payload.get(), message, parsed.command, span);
        if (span && accepted) {
            span->setSuccess();
        }
    } else {
        appLogger->warn("unable to find a handler for message type: {}", parsed.command);
        if (span) {
            span->setError("No handler registered for WebSocket command");
            span->setAttribute("websocket.message.size", static_cast<int64_t>(message ? message->size() : 0));
            span->setAttribute("websocket.command", parsed.command);
            span->setAttribute("websocket.rejection.stage", "dispatch");
            span->setAttribute("websocket.error.type", "UnknownCommand");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
    }
}

} // namespace creatures::ws
