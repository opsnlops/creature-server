
#include "blockingconcurrentqueue.h"
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "SensorReportCommandDTO.h"
#include "SensorReportHandler.h"
#include "server/database.h"
#include "server/sensors/SensorDataCache.h"
#include "util/cache.h"
#include "util/ObservabilityManager.h"

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
                        auto creatureResult = creatures::db->getCreature(creatureId);
                        if (creatureResult.isSuccess()) {
                            creatureName = creatureResult.getValue().value().name;
                            appLogger->debug("Looked up creature name from database: {} for ID: {}", creatureName,
                                             creatureId);
                        } else {
                            appLogger->warn("Failed to look up creature name for ID: {} - {}", creatureId,
                                            creatureResult.getError().value().getMessage());
                            creatureName = "Unknown Creature";
                        }
                    } else {
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