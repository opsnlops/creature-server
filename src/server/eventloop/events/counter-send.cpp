#include <spdlog/spdlog.h>

#include "blockingconcurrentqueue.h"

#include "api/WebSocketEnvelope.h"
#include "server/config.h"

#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"

#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"
#include "server/runtime/RuntimeSnapshot.h"
#include "server/ws/dto/websocket/MessageTypes.h"
#include "server/ws/service/CreatureService.h"

// Include the ObservabilityManager for metrics export
#include "server/sensors/SensorDataCache.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<SensorDataCache> sensorDataCache;

Result<framenum_t> CounterSendEvent::executeImpl() {

    if (!metrics) {
        const std::string errorMsg = "CounterSendEvent: metrics unavailable";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }
    if (!websocketOutgoingMessages) {
        const std::string errorMsg = "CounterSendEvent: websocket queue unavailable";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }
    if (!eventLoop) {
        const std::string errorMsg = "CounterSendEvent: event loop unavailable";
        error(errorMsg);
        return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
    }

    debug("sending the server metrics to all clients");

    nlohmann::json runtimeStates = nlohmann::json::array();
    for (const auto &[creatureId, runtime] : creatures::ws::CreatureService::getRuntimeStates()) {
        runtimeStates.push_back(
            {{"creature_id", creatureId}, {"runtime", runtime::creatureRuntimeSnapshotToJson(runtime)}});
    }

    const nlohmann::json payload = {{"counters", systemCountersSnapshotToJson(metrics->snapshot())},
                                    {"runtime_states", std::move(runtimeStates)}};
    const std::string messageAsString =
        api::serializeWebSocketEnvelope(toString(ws::MessageType::ServerCounters), payload);
    trace("websocket message as string: {}", messageAsString);

    // Push this into the websocket queue
    websocketOutgoingMessages->enqueue(messageAsString);

    // Now export metrics to OTel if observability is enabled
    if (observability && observability->isInitialized()) {
        trace("exporting metrics to OTel");
        observability->exportMetrics(metrics);

        // Also export sensor metrics if available
        if (sensorDataCache) {
            observability->exportSensorMetrics(sensorDataCache);
            // Clean up stale sensor data (older than 30 seconds)
            sensorDataCache->removeStaleData(30);
        }
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
