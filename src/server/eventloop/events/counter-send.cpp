#include <spdlog/spdlog.h>

#include <oatpp/core/Types.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "blockingconcurrentqueue.h"

#include "server/config.h"

#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/dto/websocket/ServerCountersMessage.h"

// Include the ObservabilityManager for metrics export
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<ObservabilityManager> observability;

Result<framenum_t> CounterSendEvent::executeImpl() {

    auto jsonMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();

    debug("sending the server metrics to all clients");

    // First, send to websocket clients
    auto message = oatpp::Object<ws::ServerCountersMessage>::createShared();
    message->command = toString(ws::MessageType::ServerCounters);
    message->payload = metrics->convertToDto();

    // Serialize our message to a string
    std::string messageAsString = jsonMapper->writeToString(message);
    trace("websocket message as string: {}", messageAsString);

    // Push this into the websocket queue
    websocketOutgoingMessages->enqueue(messageAsString);

    // Now export metrics to OTel if observability is enabled
    if (observability && observability->isInitialized()) {
        trace("exporting metrics to OTel");
        observability->exportMetrics(metrics);
    } else {
        trace("observability manager not initialized, skipping OTel metrics export");
    }

    // Schedule the next counter send event
    auto nextTick = std::make_shared<CounterSendEvent>(this->frameNumber + SEND_COUNTERS_FRAMES);
    eventLoop->scheduleEvent(nextTick);

    trace("next counter send event scheduled for frame {}", this->frameNumber + SEND_COUNTERS_FRAMES);

    return Result<framenum_t>{this->frameNumber};
}

} // namespace creatures