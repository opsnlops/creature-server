#include "server/sensors/SensorReport.h"

#include <optional>
#include <unordered_set>

#include <fmt/format.h>

#include "model/JsonCodec.h"
#include "util/helpers.h"

namespace creatures {

namespace {

template <typename Output, typename Input> Result<Output> forwardError(const Result<Input> &input) {
    return Result<Output>{input.getError().value()};
}

Result<std::optional<std::reference_wrapper<const nlohmann::json>>>
optionalArray(const nlohmann::json &json, std::string_view path, std::string_view key, std::size_t maximumEntries) {
    if (!json.contains(key) || json.at(key).is_null()) {
        return Result<std::optional<std::reference_wrapper<const nlohmann::json>>>{std::nullopt};
    }
    const auto values = json_codec::requiredArray(json, path, key, maximumEntries);
    if (!values.isSuccess()) {
        return forwardError<std::optional<std::reference_wrapper<const nlohmann::json>>>(values);
    }
    return Result<std::optional<std::reference_wrapper<const nlohmann::json>>>{values.getValue().value()};
}

Result<std::string> requiredCreatureId(const nlohmann::json &json, std::string_view path) {
    auto creatureId = json_codec::requiredString(json, path, "creature_id", 36);
    if (!creatureId.isSuccess()) {
        return creatureId;
    }
    if (!isUuidShape(creatureId.getValue().value())) {
        return json_codec::invalid<std::string>(fmt::format("{}.creature_id must be a UUID", path));
    }
    return creatureId;
}

Result<PowerSensorReading> powerReadingFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"name", "voltage", "current", "power"});
    auto name = json_codec::optionalString(json, path, "name", 128, true, true);
    auto voltage = json_codec::optionalFiniteDouble(json, path, "voltage", -1000.0, 1000.0, true);
    auto current = json_codec::optionalFiniteDouble(json, path, "current", -1000.0, 1000.0, true);
    auto power = json_codec::optionalFiniteDouble(json, path, "power", -1000000.0, 1000000.0, true);
    if (!fields.isSuccess())
        return forwardError<PowerSensorReading>(fields);
    if (!name.isSuccess())
        return forwardError<PowerSensorReading>(name);
    if (!voltage.isSuccess())
        return forwardError<PowerSensorReading>(voltage);
    if (!current.isSuccess())
        return forwardError<PowerSensorReading>(current);
    if (!power.isSuccess())
        return forwardError<PowerSensorReading>(power);
    return Result<PowerSensorReading>{PowerSensorReading{.name = name.getValue().value().value_or(""),
                                                         .voltage = voltage.getValue().value().value_or(0.0),
                                                         .current = current.getValue().value().value_or(0.0),
                                                         .power = power.getValue().value().value_or(0.0),
                                                         .lastUpdate = {}}};
}

Result<DynamixelSensorReading> dynamixelReadingFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(
        json, path, {"dxl_id", "temperature_f", "present_load", "voltage_mv", "voltage_v", "present_position"});
    auto id = json_codec::requiredUnsigned<uint32_t>(json, path, "dxl_id", 253);
    auto temperature = json_codec::optionalFiniteDouble(json, path, "temperature_f", -100.0, 500.0, true);
    auto presentLoad = json_codec::optionalInt64(json, path, "present_load", INT32_MIN, INT32_MAX, true);
    auto voltage = json_codec::optionalFiniteDouble(json, path, "voltage_v", 0.0, 100.0, true);
    auto voltageMillivolts = json_codec::optionalInt64(json, path, "voltage_mv", 0, 100000, true);
    auto position = json_codec::optionalInt64(json, path, "present_position", 0, 4095, true);
    if (!fields.isSuccess())
        return forwardError<DynamixelSensorReading>(fields);
    if (!id.isSuccess())
        return forwardError<DynamixelSensorReading>(id);
    if (!temperature.isSuccess())
        return forwardError<DynamixelSensorReading>(temperature);
    if (!presentLoad.isSuccess())
        return forwardError<DynamixelSensorReading>(presentLoad);
    if (!voltage.isSuccess())
        return forwardError<DynamixelSensorReading>(voltage);
    if (!voltageMillivolts.isSuccess())
        return forwardError<DynamixelSensorReading>(voltageMillivolts);
    if (!position.isSuccess())
        return forwardError<DynamixelSensorReading>(position);
    const auto positionValue = position.getValue().value();
    return Result<DynamixelSensorReading>{
        DynamixelSensorReading{.dxlId = id.getValue().value(),
                               .temperatureF = temperature.getValue().value().value_or(0.0),
                               .presentLoad = static_cast<int32_t>(presentLoad.getValue().value().value_or(0)),
                               .voltageV = voltage.getValue().value().value_or(0.0),
                               .presentPosition = static_cast<int32_t>(positionValue.value_or(0)),
                               .hasPosition = positionValue.has_value(),
                               .lastUpdate = {}}};
}

} // namespace

Result<BoardSensorReport> boardSensorReportFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(
        json, path, {"creature_id", "creatureName", "board_temperature", "power_reports"});
    auto creatureId = requiredCreatureId(json, path);
    auto creatureName = json_codec::optionalString(json, path, "creatureName", 256, true, true);
    auto boardTemperature = json_codec::optionalFiniteDouble(json, path, "board_temperature", -100.0, 300.0, true);
    auto powerReports = optionalArray(json, path, "power_reports", MAX_SENSOR_REPORT_READINGS);
    if (!fields.isSuccess())
        return forwardError<BoardSensorReport>(fields);
    if (!creatureId.isSuccess())
        return forwardError<BoardSensorReport>(creatureId);
    if (!creatureName.isSuccess())
        return forwardError<BoardSensorReport>(creatureName);
    if (!boardTemperature.isSuccess())
        return forwardError<BoardSensorReport>(boardTemperature);
    if (!powerReports.isSuccess())
        return forwardError<BoardSensorReport>(powerReports);

    std::vector<PowerSensorReading> readings;
    std::unordered_set<std::string> readingNames;
    const auto powerReportValues = powerReports.getValue().value();
    if (powerReportValues) {
        const auto &items = powerReportValues->get();
        readings.reserve(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto reading = powerReadingFromJson(items[index], fmt::format("{}.power_reports[{}]", path, index));
            if (!reading.isSuccess()) {
                return forwardError<BoardSensorReport>(reading);
            }
            if (!reading.getValue()->name.empty() && !readingNames.insert(reading.getValue()->name).second) {
                return json_codec::invalid<BoardSensorReport>(
                    fmt::format("{}.power_reports contains duplicate name {}", path, reading.getValue()->name));
            }
            readings.push_back(reading.getValue().value());
        }
    }
    const auto creatureNameValue = creatureName.getValue().value();
    return Result<BoardSensorReport>{
        BoardSensorReport{.creatureId = creatureId.getValue().value(),
                          .creatureName = creatureNameValue.value_or(""),
                          .boardTemperature = boardTemperature.getValue().value().value_or(0.0),
                          .powerReadings = std::move(readings)}};
}

Result<DynamixelSensorReport> dynamixelSensorReportFromJson(const nlohmann::json &json, std::string_view path) {
    auto fields = json_codec::rejectUnknownFields(json, path, {"creature_id", "creatureName", "dynamixel_motors"});
    auto creatureId = requiredCreatureId(json, path);
    auto creatureName = json_codec::optionalString(json, path, "creatureName", 256, true, true);
    auto motors = optionalArray(json, path, "dynamixel_motors", MAX_SENSOR_REPORT_READINGS);
    if (!fields.isSuccess())
        return forwardError<DynamixelSensorReport>(fields);
    if (!creatureId.isSuccess())
        return forwardError<DynamixelSensorReport>(creatureId);
    if (!creatureName.isSuccess())
        return forwardError<DynamixelSensorReport>(creatureName);
    if (!motors.isSuccess())
        return forwardError<DynamixelSensorReport>(motors);

    std::vector<DynamixelSensorReading> readings;
    std::unordered_set<uint32_t> motorIds;
    const auto motorValues = motors.getValue().value();
    if (motorValues) {
        const auto &items = motorValues->get();
        readings.reserve(items.size());
        for (std::size_t index = 0; index < items.size(); ++index) {
            auto reading = dynamixelReadingFromJson(items[index], fmt::format("{}.dynamixel_motors[{}]", path, index));
            if (!reading.isSuccess()) {
                return forwardError<DynamixelSensorReport>(reading);
            }
            if (!motorIds.insert(reading.getValue()->dxlId).second) {
                return json_codec::invalid<DynamixelSensorReport>(
                    fmt::format("{}.dynamixel_motors contains duplicate dxl_id {}", path, reading.getValue()->dxlId));
            }
            readings.push_back(reading.getValue().value());
        }
    }
    const auto dynamixelCreatureName = creatureName.getValue().value();
    return Result<DynamixelSensorReport>{DynamixelSensorReport{.creatureId = creatureId.getValue().value(),
                                                               .creatureName = dynamixelCreatureName.value_or(""),
                                                               .readings = std::move(readings)}};
}

} // namespace creatures
