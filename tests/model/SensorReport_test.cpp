#include <gtest/gtest.h>

#include "server/sensors/SensorReport.h"

namespace creatures {

namespace {

constexpr auto CREATURE_ID = "123e4567-e89b-12d3-a456-426614174000";

nlohmann::json validPowerReading() { return {{"name", "vbus"}, {"voltage", 12.3}, {"current", 1.5}, {"power", 18.45}}; }

nlohmann::json validDynamixelReading() {
    return {{"dxl_id", 4},         {"temperature_f", 88.5}, {"present_load", -22},
            {"voltage_mv", 12000}, {"voltage_v", 12.0},     {"present_position", 2048}};
}

} // namespace

TEST(SensorReportJson, ParsesBoundedBoardTelemetry) {
    const auto parsed = boardSensorReportFromJson({{"creature_id", CREATURE_ID},
                                                   {"creatureName", "Test Creature"},
                                                   {"board_temperature", 72.5},
                                                   {"power_reports", {validPowerReading()}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->creatureId, CREATURE_ID);
    EXPECT_EQ(parsed.getValue()->creatureName, "Test Creature");
    EXPECT_DOUBLE_EQ(parsed.getValue()->boardTemperature, 72.5);
    ASSERT_EQ(parsed.getValue()->powerReadings.size(), 1);
    EXPECT_EQ(parsed.getValue()->powerReadings.front().name, "vbus");
}

TEST(SensorReportJson, RejectsInvalidOrOversizedBoardTelemetry) {
    const nlohmann::json valid = {
        {"creature_id", CREATURE_ID}, {"board_temperature", 72.5}, {"power_reports", {validPowerReading()}}};
    auto invalidId = valid;
    invalidId["creature_id"] = "not-a-uuid";
    EXPECT_FALSE(boardSensorReportFromJson(invalidId).isSuccess());

    auto unknownReadingField = valid;
    unknownReadingField["power_reports"][0]["extra"] = true;
    EXPECT_FALSE(boardSensorReportFromJson(unknownReadingField).isSuccess());

    auto tooManyReadings = valid;
    tooManyReadings["power_reports"] = nlohmann::json::array();
    for (std::size_t index = 0; index <= MAX_SENSOR_REPORT_READINGS; ++index) {
        tooManyReadings["power_reports"].push_back(validPowerReading());
    }
    EXPECT_FALSE(boardSensorReportFromJson(tooManyReadings).isSuccess());

    auto duplicateName = valid;
    duplicateName["power_reports"].push_back(validPowerReading());
    EXPECT_FALSE(boardSensorReportFromJson(duplicateName).isSuccess());
}

TEST(SensorReportJson, PreservesPartialLegacyTelemetryDefaults) {
    const auto board = boardSensorReportFromJson({{"creature_id", CREATURE_ID}});
    ASSERT_TRUE(board.isSuccess()) << board.getError()->getMessage();
    EXPECT_DOUBLE_EQ(board.getValue()->boardTemperature, 0.0);
    EXPECT_TRUE(board.getValue()->powerReadings.empty());

    const auto dynamixel = dynamixelSensorReportFromJson(
        {{"creature_id", CREATURE_ID}, {"dynamixel_motors", {{{"dxl_id", 4}, {"voltage_mv", 12000}}}}});
    ASSERT_TRUE(dynamixel.isSuccess()) << dynamixel.getError()->getMessage();
    ASSERT_EQ(dynamixel.getValue()->readings.size(), 1);
    EXPECT_DOUBLE_EQ(dynamixel.getValue()->readings.front().voltageV, 0.0);

    const auto nulls = boardSensorReportFromJson(
        {{"creature_id", CREATURE_ID}, {"creatureName", nullptr}, {"board_temperature", nullptr}});
    ASSERT_TRUE(nulls.isSuccess()) << nulls.getError()->getMessage();
    EXPECT_EQ(nulls.getValue()->creatureName, "");
}

TEST(SensorReportJson, ParsesDynamixelOptionalPosition) {
    auto reading = validDynamixelReading();
    reading.erase("present_position");
    const auto parsed =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {std::move(reading)}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    const auto report = parsed.getValue().value();
    ASSERT_EQ(report.readings.size(), 1);
    const auto &motor = report.readings.front();
    EXPECT_EQ(motor.dxlId, 4U);
    EXPECT_EQ(motor.presentLoad, -22);
    EXPECT_FALSE(motor.hasPosition);
    EXPECT_EQ(motor.presentPosition, 0);
}

TEST(SensorReportJson, RejectsInvalidDynamixelTelemetry) {
    const nlohmann::json valid = {{"creature_id", CREATURE_ID}, {"dynamixel_motors", {validDynamixelReading()}}};
    auto invalidPosition = valid;
    invalidPosition["dynamixel_motors"][0]["present_position"] = 4096;
    EXPECT_FALSE(dynamixelSensorReportFromJson(invalidPosition).isSuccess());

    auto invalidId = valid;
    invalidId["dynamixel_motors"][0]["dxl_id"] = -1;
    EXPECT_FALSE(dynamixelSensorReportFromJson(invalidId).isSuccess());

    auto duplicateId = valid;
    duplicateId["dynamixel_motors"].push_back(validDynamixelReading());
    EXPECT_FALSE(dynamixelSensorReportFromJson(duplicateId).isSuccess());
}

} // namespace creatures
