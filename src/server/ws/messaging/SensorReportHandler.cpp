#include "blockingconcurrentqueue.h"
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>

#include "SensorReportHandler.h"
#include "server/database.h"
#include "server/sensors/SensorDataCache.h"
#include "server/sensors/SensorReport.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<SensorDataCache> sensorDataCache;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<Database> db;
} // namespace creatures

namespace creatures::ws {

bool SensorReportHandler::processMessage(const nlohmann::json &payload, std::string_view message,
                                         std::string_view command, std::shared_ptr<SamplingSpan> messageSpan) {
    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    if (messageSpan) {
        messageSpan->setAttribute("websocket.command", std::string(command));
        messageSpan->setAttribute("websocket.handler", "sensor-report");
        messageSpan->setAttribute("websocket.message.size", static_cast<int64_t>(message.size()));
    }

    try {
        const auto parsed = boardSensorReportFromJson(payload);
        if (!parsed.isSuccess()) {
            const auto errorMessage = parsed.getError()->getMessage();
            appLogger->warn("Rejected board sensor report: {}", errorMessage);
            if (messageSpan) {
                messageSpan->setError(errorMessage);
                messageSpan->setAttribute("websocket.rejection.stage", "payload");
                messageSpan->setAttribute("error.type", "InvalidSensorReport");
                messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
            }
        } else {
            const auto parsedValue = parsed.getValue();
            const auto &report = parsedValue.value();
            std::string creatureName;

            if (creatures::creatureCache) {
                try {
                    creatureName = creatures::creatureCache->get(report.creatureId)->name;
                    if (messageSpan) {
                        messageSpan->setAttribute("cache.creature.hit", true);
                    }
                } catch (const std::out_of_range &) {
                    if (messageSpan) {
                        messageSpan->setAttribute("cache.creature.hit", false);
                    }
                    if (creatures::db) {
                        std::shared_ptr<OperationSpan> operationSpan = messageSpan;
                        const auto creatureResult = creatures::db->getCreature(report.creatureId, operationSpan);
                        if (creatureResult.isSuccess()) {
                            creatureName = creatureResult.getValue()->name;
                        } else {
                            appLogger->warn("Failed to look up creature name for ID: {} - {}", report.creatureId,
                                            creatureResult.getError()->getMessage());
                        }
                    }
                }
            }
            if (creatures::creatureCache) {
                if (creatureName.empty()) {
                    creatureName = "Unknown Creature";
                }
            } else if (creatureName.empty()) {
                creatureName = report.creatureName.empty() ? "Unknown Creature" : report.creatureName;
            }

            if (messageSpan) {
                messageSpan->setAttribute("sensor.report.phase", "caching");
                messageSpan->setAttribute("creature.id", report.creatureId);
                messageSpan->setAttribute("creature.name", creatureName);
                messageSpan->setAttribute("board.temperature", report.boardTemperature);
                messageSpan->setAttribute("sensor.power_reading.count",
                                          static_cast<int64_t>(report.powerReadings.size()));
            }
            if (creatures::sensorDataCache) {
                creatures::sensorDataCache->updateSensorData(report.creatureId, creatureName, report.boardTemperature,
                                                             report.powerReadings);
            }
            appLogger->debug("Updated sensor cache for creature {} ({}): temp={:.2f}F, {} power readings", creatureName,
                             report.creatureId, report.boardTemperature, report.powerReadings.size());
            if (messageSpan) {
                messageSpan->setSuccess();
            }
            if (messageSpan) {
                messageSpan->setAttribute("websocket.broadcast.enqueued", true);
            }
            websocketOutgoingMessages->enqueue(std::string(message));
            return true;
        }
    } catch (const std::exception &error) {
        const auto errorMessage = fmt::format("Exception while processing board sensor report: {}", error.what());
        appLogger->warn(errorMessage);
        if (messageSpan) {
            messageSpan->recordException(error);
            messageSpan->setError(errorMessage);
            messageSpan->setAttribute("error.type", "std::exception");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    } catch (...) {
        constexpr std::string_view errorMessage = "Unknown error while processing board sensor report";
        appLogger->warn(errorMessage);
        if (messageSpan) {
            messageSpan->setError(std::string(errorMessage));
            messageSpan->setAttribute("error.type", "unknown");
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    }

    // Preserve the existing broadcast contract: controllers' reports are
    // forwarded even when their metrics payload is rejected locally.
    if (messageSpan) {
        messageSpan->setAttribute("sensor.report.phase", "forwarding");
        messageSpan->setAttribute("websocket.broadcast.enqueued", true);
    }
    websocketOutgoingMessages->enqueue(std::string(message));
    return false;
}

} // namespace creatures::ws
