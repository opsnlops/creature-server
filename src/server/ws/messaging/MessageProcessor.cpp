

#include <spdlog/spdlog.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>

#include "server/ws/dto/websocket/MessageTypes.h"

#include "MessageProcessor.h"
#include "NoticeMessageHandler.h"
#include "SensorReportHandler.h"
#include "StreamFrameHandler.h"

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

    // Log how many we have total
    appLogger->info("{} message handler{} registered", handlers.size(), handlers.size() != 1 ? "s" : "");
}

void MessageProcessor::processIncomingMessage(const std::string &command, const oatpp::String &message) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    // Find the right handler
    if (handlers.find(command) != handlers.end()) {
        handlers[command]->processMessage(message);
    } else {
        appLogger->warn("unable to find a handler for message type: {}", command);
    }
}

} // namespace creatures::ws