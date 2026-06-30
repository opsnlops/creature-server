#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "server/namespace-stuffs.h"

namespace creatures {

/**
 * Individual sensor reading for power monitoring
 */
struct PowerSensorReading {
    std::string name; // "vbus", "motor_power_in", "3v3", etc.
    double voltage;   // Voltage in volts
    double current;   // Current in amperes
    double power;     // Power in watts
    std::chrono::system_clock::time_point lastUpdate;
};

/**
 * Complete sensor data for a single creature
 */
struct CreatureSensorData {
    creatureId_t creatureId;
    std::string creatureName;
    double boardTemperature; // Temperature in Fahrenheit
    std::vector<PowerSensorReading> powerReadings;
    std::chrono::system_clock::time_point lastUpdate;
};

/**
 * Individual Dynamixel servo telemetry reading
 */
struct DynamixelSensorReading {
    uint32_t dxlId;          // Dynamixel servo ID on the bus
    double temperatureF;     // Servo temperature in Fahrenheit
    int32_t presentLoad;     // Present load (raw, signed)
    double voltageV;         // Input voltage in volts
    int32_t presentPosition; // Raw encoder position (0-4095 for XC430-class servos)
    bool hasPosition;        // False on older firmware that omits present_position
    std::chrono::system_clock::time_point lastUpdate;
};

/**
 * Complete Dynamixel telemetry for a single creature
 *
 * Stored separately from CreatureSensorData because Dynamixel reports arrive
 * as their own WebSocket message at a different cadence than board reports;
 * keeping them apart avoids one report clobbering the other's cached data.
 */
struct CreatureDynamixelData {
    creatureId_t creatureId;
    std::string creatureName;
    std::vector<DynamixelSensorReading> dynamixelReadings;
    std::chrono::system_clock::time_point lastUpdate;
};

/**
 * Cache for storing current sensor data from all creatures
 * Thread-safe cache that stores the latest sensor readings from each creature
 */
class SensorDataCache {
  public:
    SensorDataCache();
    ~SensorDataCache() = default;

    /**
     * Update sensor data for a specific creature
     * @param creatureId The ID of the creature
     * @param creatureName The name of the creature
     * @param boardTemperature Temperature reading in Fahrenheit
     * @param powerReadings Vector of power sensor readings
     */
    void updateSensorData(creatureId_t creatureId, const std::string &creatureName, double boardTemperature,
                          const std::vector<PowerSensorReading> &powerReadings);

    /**
     * Update Dynamixel telemetry for a specific creature
     * @param creatureId The ID of the creature
     * @param creatureName The name of the creature
     * @param dynamixelReadings Vector of per-servo Dynamixel readings
     */
    void updateDynamixelData(creatureId_t creatureId, const std::string &creatureName,
                             const std::vector<DynamixelSensorReading> &dynamixelReadings);

    /**
     * Get current sensor data for all creatures
     * @return Map of creature ID to sensor data
     */
    std::unordered_map<creatureId_t, CreatureSensorData> getAllSensorData() const;

    /**
     * Get current Dynamixel telemetry for all creatures
     * @return Map of creature ID to Dynamixel data
     */
    std::unordered_map<creatureId_t, CreatureDynamixelData> getAllDynamixelData() const;

    /**
     * Get sensor data for a specific creature
     * @param creatureId The creature ID to look up
     * @return Optional sensor data, empty if not found
     */
    std::optional<CreatureSensorData> getSensorData(creatureId_t creatureId) const;

    /**
     * Check if we have recent data for a creature (within the last 10 seconds)
     * @param creatureId The creature ID to check
     * @return True if we have recent data
     */
    bool hasRecentData(creatureId_t creatureId) const;

    /**
     * Remove stale sensor data (older than timeout)
     * @param timeoutSeconds Remove data older than this many seconds
     */
    void removeStaleData(int timeoutSeconds = 30);

  private:
    mutable std::mutex cacheMutex_;
    std::unordered_map<creatureId_t, CreatureSensorData> sensorData_;
    std::unordered_map<creatureId_t, CreatureDynamixelData> dynamixelData_;
};

} // namespace creatures