
#include "blockingconcurrentqueue.h"
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "SensorReportCommandDTO.h"
#include "SensorReportHandler.h"
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

void SensorReportHandler::processMessage(const oatpp::String &message) {

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    // Create a sampling span for this operation (uses default 0.1% sampling rate)
    auto messageSpan = creatures::observability->createSamplingSpan("SensorReportHandler.processMessage");

    try {
        appLogger->debug("processing an incoming SensorReport message");

        // Parse the JSON message into our DTO
        auto objectMapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        auto dto = objectMapper->readFromString<oatpp::Object<creatures::ws::BoardSensorReportCommandDTO>>(message);

        if (dto && dto->payload) {
            messageSpan->setAttribute("phase", "parsing");

            // Extract creature info
            auto creatureId = dto->payload->creatureId ? std::string(dto->payload->creatureId) : "";
            auto boardTemperature =
                dto->payload->board_temperature ? static_cast<double>(dto->payload->board_temperature) : 0.0;

            // Look up creature name from cache (with database fallback) if we have a valid creature ID
            std::string creatureName;
            appLogger->debug("Creature lookup: creatureId='{}' (len={}), creatureCache={}", creatureId,
                             creatureId.length(), creatures::creatureCache ? "available" : "null");

            if (!creatureId.empty() && creatures::creatureCache) {
                try {
                    auto creature = creatures::creatureCache->get(creatureId);
                    creatureName = creature->name;
                    messageSpan->setAttribute("creature_cache.hit", true);
                    appLogger->debug("Looked up creature name from cache: {} for ID: {}", creatureName, creatureId);
                } catch (const std::out_of_range &e) {
                    messageSpan->setAttribute("creature_cache.hit", false);
                    appLogger->debug("Creature {} not found in cache, trying database...", creatureId);

                    if (creatures::db) {
                        appLogger->debug("Starting database creature lookup for ID: {}", creatureId);

                        // ======================================================================
                        // 🚨 INHERITANCE CHECK: SamplingSpan -> OperationSpan conversion 🚨
                        // This should work seamlessly due to inheritance, but let's be explicit
                        // ======================================================================
                        std::shared_ptr<OperationSpan> operationSpan = messageSpan;
                        if (!operationSpan) {
                            appLogger->critical(
                                "🚨 FATAL: messageSpan failed to convert to OperationSpan! Inheritance broken!");
                            messageSpan->setError("Failed to convert SamplingSpan to OperationSpan");
                            creatureName = "INHERITANCE_ERROR";
                        } else {
                            appLogger->debug(
                                "✅ SamplingSpan successfully converted to OperationSpan for database call");

                            auto creatureResult = creatures::db->getCreature(creatureId, operationSpan);
                            appLogger->debug("Database creature lookup completed for ID: {}, success: {}", creatureId,
                                             creatureResult.isSuccess());

                            if (creatureResult.isSuccess()) {
                                auto creature = creatureResult.getValue().value();
                                creatureName = creature.name;
                                appLogger->debug("Successfully looked up creature from database: '{}' (ID: {}, "
                                                 "audio_channel: {}, channel_offset: {})",
                                                 creatureName, creature.id, creature.audio_channel,
                                                 creature.channel_offset);
                            } else {
                                appLogger->warn("Failed to look up creature name for ID: {} - {}", creatureId,
                                                creatureResult.getError().value().getMessage());
                                creatureName = "Unknown Creature";
                            }
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

            // Convert power readings
            std::vector<PowerSensorReading> powerReadings;
            if (dto->payload->power_reports) {
                powerReadings.reserve(dto->payload->power_reports->size());
                for (const auto &powerDto : *dto->payload->power_reports) {
                    if (powerDto) {
                        PowerSensorReading reading;
                        reading.name = powerDto->name ? std::string(powerDto->name) : "";
                        reading.voltage = powerDto->voltage ? static_cast<double>(powerDto->voltage) : 0.0;
                        reading.current = powerDto->current ? static_cast<double>(powerDto->current) : 0.0;
                        reading.power = powerDto->power ? static_cast<double>(powerDto->power) : 0.0;
                        powerReadings.push_back(reading);
                    }
                }
            }

            messageSpan->setAttribute("phase", "caching");
            messageSpan->setAttribute("creature.id", creatureId);
            messageSpan->setAttribute("creature.name", creatureName);
            messageSpan->setAttribute("board.temperature", boardTemperature);
            messageSpan->setAttribute("power_readings.count", static_cast<int64_t>(powerReadings.size()));

            // Store in sensor data cache for metrics export
            if (creatures::sensorDataCache) {
                appLogger->debug("About to update sensor cache: creatureId='{}' (len={}), creatureName='{}' (len={})",
                                 creatureId, creatureId.length(), creatureName, creatureName.length());
                creatures::sensorDataCache->updateSensorData(creatureId, creatureName, boardTemperature, powerReadings);
                appLogger->debug("Updated sensor cache for creature {} ({}): temp={:.2f}F, {} power readings",
                                 creatureName, creatureId, boardTemperature, powerReadings.size());
            }

            messageSpan->setSuccess();
        } else {
            appLogger->warn("unable to parse incoming sensor report message as BoardSensorReportCommandDTO");
            messageSpan->setError("Failed to parse sensor report DTO");
        }

        // Always forward the message to connected clients (original behavior)
        messageSpan->setAttribute("phase", "forwarding");
        websocketOutgoingMessages->enqueue(message);

    } catch (const std::bad_cast &e) {
        auto errorMessage = fmt::format("Error (std::bad_cast) while processing sensor report '{}': {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::bad_cast");
        messageSpan->setAttribute("error.message", errorMessage);

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    } catch (const std::exception &e) {
        auto errorMessage = fmt::format("Error (std::exception) while processing sensor report '{}': {}",
                                        std::string(message), e.what());
        appLogger->warn(errorMessage);
        messageSpan->recordException(e);
        messageSpan->setAttribute("error.type", "std::exception");
        messageSpan->setAttribute("error.message", errorMessage);

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    } catch (...) {
        auto errorMessage = fmt::format("Unknown error while processing sensor report '{}'", std::string(message));
        appLogger->warn(errorMessage);
        messageSpan->setError(errorMessage);
        messageSpan->setAttribute("error.type", "unknown");

        // Still forward message to clients even if parsing failed
        websocketOutgoingMessages->enqueue(message);
    }
}
} // namespace creatures::ws