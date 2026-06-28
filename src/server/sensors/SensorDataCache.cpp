#include "SensorDataCache.h"

#include <mutex>

namespace creatures {

SensorDataCache::SensorDataCache() = default;

void SensorDataCache::updateSensorData(creatureId_t creatureId, const std::string &creatureName,
                                       double boardTemperature, const std::vector<PowerSensorReading> &powerReadings) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    auto now = std::chrono::system_clock::now();

    // Update all power readings with current timestamp
    std::vector<PowerSensorReading> timestampedReadings;
    timestampedReadings.reserve(powerReadings.size());
    for (const auto &reading : powerReadings) {
        PowerSensorReading timestamped = reading;
        timestamped.lastUpdate = now;
        timestampedReadings.push_back(timestamped);
    }

    sensorData_[creatureId] = {
        .creatureId = creatureId,
        .creatureName = creatureName,
        .boardTemperature = boardTemperature,
        .powerReadings = std::move(timestampedReadings),
        .lastUpdate = now,
    };
}

void SensorDataCache::updateDynamixelData(creatureId_t creatureId, const std::string &creatureName,
                                          const std::vector<DynamixelSensorReading> &dynamixelReadings) {
    std::lock_guard<std::mutex> lock(cacheMutex_);

    auto now = std::chrono::system_clock::now();

    // Stamp each reading with the current time
    std::vector<DynamixelSensorReading> timestampedReadings;
    timestampedReadings.reserve(dynamixelReadings.size());
    for (const auto &reading : dynamixelReadings) {
        DynamixelSensorReading timestamped = reading;
        timestamped.lastUpdate = now;
        timestampedReadings.push_back(timestamped);
    }

    dynamixelData_[creatureId] = {
        .creatureId = creatureId,
        .creatureName = creatureName,
        .dynamixelReadings = std::move(timestampedReadings),
        .lastUpdate = now,
    };
}

std::unordered_map<creatureId_t, CreatureSensorData> SensorDataCache::getAllSensorData() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return sensorData_;
}

std::unordered_map<creatureId_t, CreatureDynamixelData> SensorDataCache::getAllDynamixelData() const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    return dynamixelData_;
}

std::optional<CreatureSensorData> SensorDataCache::getSensorData(creatureId_t creatureId) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = sensorData_.find(creatureId);
    if (it != sensorData_.end()) {
        return it->second;
    }
    return std::nullopt;
}

bool SensorDataCache::hasRecentData(creatureId_t creatureId) const {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto it = sensorData_.find(creatureId);
    if (it == sensorData_.end()) {
        return false;
    }

    auto now = std::chrono::system_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastUpdate);
    return elapsed.count() <= 10; // Within last 10 seconds
}

void SensorDataCache::removeStaleData(int timeoutSeconds) {
    std::lock_guard<std::mutex> lock(cacheMutex_);
    auto now = std::chrono::system_clock::now();

    auto it = sensorData_.begin();
    while (it != sensorData_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.lastUpdate);
        if (elapsed.count() > timeoutSeconds) {
            it = sensorData_.erase(it);
        } else {
            ++it;
        }
    }

    auto dxlIt = dynamixelData_.begin();
    while (dxlIt != dynamixelData_.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - dxlIt->second.lastUpdate);
        if (elapsed.count() > timeoutSeconds) {
            dxlIt = dynamixelData_.erase(dxlIt);
        } else {
            ++dxlIt;
        }
    }
}

} // namespace creatures