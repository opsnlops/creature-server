
#include "blockingconcurrentqueue.h"
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "DynamixelSensorReportCommandDTO.h"
#include "DynamixelSensorReportHandler.h"
#include "server/database.h"
#include "server/sensors/SensorDataCache.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<SensorDataCache> sensorDataCache;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<Database> db;
} // namespace creatures

namespace creatures ::ws {

void DynamixelSensorReportHandler::processMessage(const oatpp::String &message) {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    // Create a sampling span for this operation (uses default 0.1% sampling rate)
    auto messageSpan = creatures::observability->createSamplingSpan("DynamixelSensorReportHandler.processMessage");

    try {
        appLogger->debug("processing an incoming DynamixelSensorReport message");

        // Parse the JSON message into our DTO
        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        auto dto = objectMapper->readFromString<oatpp::Object<creatures::ws::DynamixelSensorReportCommandDTO>>(message);

        if (dto && dto->payload) {
            messageSpan->setAttribute("phase", "parsing");

            // Extract creature info
            auto creatureId = dto->payload->creatureId ? std::string(dto->payload->creatureId) : "";

            // Look up creature name from cache (with database fallback) if we have a valid creature ID
            std::string creatureName;
            if (!creatureId.empty() && creatures::creatureCache) {
                try {
                    auto creature = creatures::creatureCache->get(creatureId);
                    creatureName = creature->name;
                    messageSpan->setAttribute("creature_cache.hit", true);
                } catch (const std::out_of_range &e) {
                    messageSpan->setAttribute("creature_cache.hit", false);
                    appLogger->debug("Creature {} not found in cache, trying database...", creatureId);

                    if (creatures::db) {
                        std::shared_ptr<OperationSpan> operationSpan = messageSpan;
                        auto creatureResult = creatures::db->getCreature(creatureId, operationSpan);
                        if (creatureResult.isSuccess()) {
                            creatureName = creatureResult.getValue().value().name;
                        } else {
                            appLogger->warn("Failed to look up creature name for ID: {} - {}", creatureId,
                                            creatureResult.getError().value().getMessage());
                            creatureName = "Unknown Creature";
                        }
                    } else {
                        appLogger->warn("Database connection not available, cannot lookup creature ID: {}", creatureId);
                        creatureName = "Unknown Creature";
                    }
                }
            } else {
                creatureName =
                    dto->payload->creatureName ? std::string(dto->payload->creatureName) : "Unknown Creature";
            }

            // Convert Dynamixel readings
            std::vector<DynamixelSensorReading> dynamixelReadings;
            if (dto->payload->dynamixel_motors) {
                dynamixelReadings.reserve(dto->payload->dynamixel_motors->size());
                for (const auto &motorDto : *dto->payload->dynamixel_motors) {
                    if (motorDto) {
                        DynamixelSensorReading reading;
                        reading.dxlId = motorDto->dxl_id ? static_cast<uint32_t>(motorDto->dxl_id) : 0;
                        reading.temperatureF =
                            motorDto->temperature_f ? static_cast<double>(motorDto->temperature_f) : 0.0;
                        reading.presentLoad = motorDto->present_load ? static_cast<int32_t>(motorDto->present_load) : 0;
                        reading.voltageV = motorDto->voltage_v ? static_cast<double>(motorDto->voltage_v) : 0.0;
                        // present_position is absent on older firmware; only treat it as present
                        // when the field actually arrived in the payload
                        reading.hasPosition = static_cast<bool>(motorDto->present_position);
                        reading.presentPosition =
                            reading.hasPosition ? static_cast<int32_t>(motorDto->present_position) : 0;
                        dynamixelReadings.push_back(reading);
                    }
                }
            }

            messageSpan->setAttribute("phase", "caching");
            messageSpan->setAttribute("creature.id", creatureId);
            messageSpan->setAttribute("creature.name", creatureName);
            messageSpan->setAttribute("dynamixel_readings.count", static_cast<int64_t>(dynamixelReadings.size()));

            // Store in sensor data cache for metrics export
            if (creatures::sensorDataCache) {
                creatures::sensorDataCache->updateDynamixelData(creatureId, creatureName, dynamixelReadings);
                appLogger->debug("Updated dynamixel cache for creature {} ({}): {} servo readings", creatureName,
                                 creatureId, dynamixelReadings.size());
            }

            messageSpan->setSuccess();
        } else {
            appLogger->warn("unable to parse incoming message as DynamixelSensorReportCommandDTO");
            messageSpan->setError("Failed to parse dynamixel sensor report DTO");
        }

        // Always forward the message to connected clients
        messageSpan->setAttribute("phase", "forwarding");
        websocketOutgoingMessages->enqueue(message);

    } catch (const std::bad_cast &e) {
        auto errorMessage = fmt::format("Error (std::bad_cast) while processing dynamixel sensor report '{}': {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::bad_cast");
        messageSpan->setAttribute("error.message", errorMessage);

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    } catch (const std::exception &e) {
        auto errorMessage = fmt::format("Error (std::exception) while processing dynamixel sensor report '{}': {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::exception");
        messageSpan->setAttribute("error.message", errorMessage);

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    } catch (...) {
        auto errorMessage =
            fmt::format("Unknown error while processing dynamixel sensor report '{}'", std::string(message));
        appLogger->warn(errorMessage);
        messageSpan->setError(errorMessage);
        messageSpan->setAttribute("error.type", "unknown");

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    }
}
} // namespace creatures::ws
