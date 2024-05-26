
#include <spdlog/spdlog.h>

#include <oatpp/parser/json/mapping/ObjectMapper.hpp>
#include <oatpp/core/Types.hpp>

#include "blockingconcurrentqueue.h"

#include "server/config.h"

#include "server/eventloop/events/types.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/event.h"

#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/ServerCountersMessage.h"

namespace creatures {

    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;

    void CounterSendEvent::executeImpl() {

        auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

        debug("sending the server metrics to all clients");

        // Send our counter to any web socket client that happens to be connected
        auto message = oatpp::Object<ws::ServerCountersMessage>::createShared();
        message->command = toString(ws::MessageType::ServerCounters);
        message->payload = metrics->convertToDto();

        // Serialize our message to a string
        std::string messageAsString = jsonMapper->writeToString(message);
        trace("message as string: {}", messageAsString);

        // Push this into the queue
        websocketOutgoingMessages->enqueue(messageAsString);

        // Make another event
        auto nextTick = std::make_shared<CounterSendEvent>(this->frameNumber + SEND_COUNTERS_FRAMES);
        eventLoop->scheduleEvent(nextTick);
    }

}