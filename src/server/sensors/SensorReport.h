#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/sensors/SensorDataCache.h"
#include "util/Result.h"

namespace creatures {

inline constexpr std::size_t MAX_SENSOR_REPORT_READINGS = 64;

struct BoardSensorReport {
    creatureId_t creatureId;
    std::string creatureName;
    double boardTemperature;
    std::vector<PowerSensorReading> powerReadings;
};

struct DynamixelSensorReport {
    creatureId_t creatureId;
    std::string creatureName;
    std::vector<DynamixelSensorReading> readings;
};

Result<BoardSensorReport> boardSensorReportFromJson(const nlohmann::json &json,
                                                    std::string_view path = "board_sensor_report");
Result<DynamixelSensorReport> dynamixelSensorReportFromJson(const nlohmann::json &json,
                                                            std::string_view path = "dynamixel_sensor_report");

} // namespace creatures
