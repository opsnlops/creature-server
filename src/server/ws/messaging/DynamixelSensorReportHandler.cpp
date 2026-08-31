#include "DynamixelSensorReportHandler.h"
#include "blockingconcurrentqueue.h"
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

bool DynamixelSensorReportHandler::processMessage(const nlohmann::json &payload, std::string_view message,
                                                  std::string_view command, std::shared_ptr<SamplingSpan> messageSpan) {
    if (messageSpan) {
        messageSpan->setAttribute("websocket.command", std::string(command));
        messageSpan->setAttribute("websocket.handler", "dynamixel-sensor-report");
        messageSpan->setAttribute("websocket.message.size", static_cast<int64_t>(message.size()));
    }

    try {
        const auto parsed = dynamixelSensorReportFromJson(payload);
        if (!parsed.isSuccess()) {
            const auto errorMessage = parsed.getError()->getMessage();
            logger_->warn("Rejected Dynamixel sensor report: {}", errorMessage);
            if (messageSpan) {
                messageSpan->setError(errorMessage);
                messageSpan->setAttribute("websocket.rejection.stage", "payload");
                messageSpan->setAttribute("error.type", "InvalidDynamixelSensorReport");
                messageSpan->setAttribute("error.message", errorMessage);
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
                        messageSpan->setAttribute("creature.cache.hit", true);
                    }
                } catch (const std::out_of_range &) {
                    if (messageSpan) {
                        messageSpan->setAttribute("creature.cache.hit", false);
                    }
                    if (creatures::db) {
                        std::shared_ptr<OperationSpan> operationSpan = messageSpan;
                        const auto creatureResult = creatures::db->getCreature(report.creatureId, operationSpan);
                        if (creatureResult.isSuccess()) {
                            creatureName = creatureResult.getValue()->name;
                        } else {
                            logger_->warn("Failed to look up creature name for ID: {} - {}", report.creatureId,
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
                messageSpan->setAttribute("sensor.dynamixel_reading.count",
                                          static_cast<int64_t>(report.readings.size()));
            }
            if (creatures::sensorDataCache) {
                creatures::sensorDataCache->updateDynamixelData(report.creatureId, creatureName, report.readings);
            }
            logger_->debug("Updated Dynamixel cache for creature {} ({}): {} servo readings", creatureName,
                           report.creatureId, report.readings.size());
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
        const auto errorMessage = fmt::format("Exception while processing Dynamixel sensor report: {}", error.what());
        logger_->warn(errorMessage);
        if (messageSpan) {
            messageSpan->recordException(error);
            messageSpan->setError(errorMessage);
            messageSpan->setAttribute("error.type", "std::exception");
            messageSpan->setAttribute("error.message", errorMessage);
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    } catch (...) {
        constexpr std::string_view errorMessage = "Unknown error while processing Dynamixel sensor report";
        logger_->warn(errorMessage);
        if (messageSpan) {
            messageSpan->setError(std::string(errorMessage));
            messageSpan->setAttribute("error.type", "unknown");
            messageSpan->setAttribute("error.message", std::string(errorMessage));
            messageSpan->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
    }

    if (messageSpan) {
        messageSpan->setAttribute("sensor.report.phase", "forwarding");
        messageSpan->setAttribute("websocket.broadcast.enqueued", true);
    }
    websocketOutgoingMessages->enqueue(std::string(message));
    return false;
}

} // namespace creatures::ws
