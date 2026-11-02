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

    const auto nulls = boardSensorReportFromJson({{"creature_id", CREATURE_ID},
                                                  {"creatureName", nullptr},
                                                  {"board_temperature", nullptr},
                                                  {"power_reports", nullptr}});
    ASSERT_TRUE(nulls.isSuccess()) << nulls.getError()->getMessage();
    EXPECT_EQ(nulls.getValue()->creatureName, "");

    const auto dynamixelNull =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", nullptr}});
    ASSERT_TRUE(dynamixelNull.isSuccess()) << dynamixelNull.getError()->getMessage();
    EXPECT_TRUE(dynamixelNull.getValue()->readings.empty());
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

// --- issue #229: the controller's `online` flag ---

TEST(SensorReportJson, AcceptsTheOnlineFlagFromTheWorkbenchRig) {
    // The exact shape the controller sends (DynamixelSensorHandler.cpp emits
    // `online` unconditionally). Before #229 this whole report was rejected
    // with "contains unknown field 'online'", which left the rig invisible to
    // the sensor cache and the OTel gauges.
    auto reading = validDynamixelReading();
    reading["online"] = true;
    const auto parsed =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {std::move(reading)}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    ASSERT_EQ(parsed.getValue()->readings.size(), 1);
    EXPECT_TRUE(parsed.getValue()->readings.front().online);
}

TEST(SensorReportJson, ParsesAnOfflineServoWithoutLosingTheReport) {
    // The firmware reports EVERY configured servo every cycle, sending
    // `D<id> 0.0 0 0 0 0` for one it did not hear from, so an absent servo is
    // visibly absent rather than frozen at its last good reading. The report
    // must still parse, and the flag must survive for consumers to act on.
    nlohmann::json offline = {{"dxl_id", 7},      {"temperature_f", 0.0},  {"present_load", 0}, {"voltage_mv", 0},
                              {"voltage_v", 0.0}, {"present_position", 0}, {"online", false}};
    const auto parsed =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {std::move(offline)}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    // Copy the report out first: Result::getValue() returns BY VALUE, so a
    // reference into `parsed.getValue()->readings` dangles at the end of the
    // statement that created the temporary.
    const auto report = parsed.getValue().value();
    ASSERT_EQ(report.readings.size(), 1);
    const auto &motor = report.readings.front();
    EXPECT_EQ(motor.dxlId, 7U);
    EXPECT_FALSE(motor.online) << "an offline servo must not be reported as online";
    EXPECT_EQ(motor.temperatureF, 0.0) << "its zeros are preserved verbatim; online is what makes them readable";
}

TEST(SensorReportJson, OmittedOnlineFlagDefaultsToOnline) {
    // Firmware older than the flag omits it, and the controller defaults that
    // case to true as well. Defaulting to false here would mark every healthy
    // servo on an un-upgraded bus as offline and suppress all of its gauges.
    auto reading = validDynamixelReading();
    ASSERT_FALSE(reading.contains("online"));
    const auto parsed =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {std::move(reading)}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    ASSERT_EQ(parsed.getValue()->readings.size(), 1);
    EXPECT_TRUE(parsed.getValue()->readings.front().online);
}

TEST(SensorReportJson, NullOnlineFlagDefaultsToOnline) {
    auto reading = validDynamixelReading();
    reading["online"] = nullptr;
    const auto parsed =
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {std::move(reading)}}});

    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_TRUE(parsed.getValue()->readings.front().online);
}

TEST(SensorReportJson, RejectsANonBooleanOnlineFlag) {
    // Accepting the field must not mean accepting anything in it — a string or
    // number here means the controller contract drifted again, and that should
    // be loud rather than silently coerced.
    auto numeric = validDynamixelReading();
    numeric["online"] = 1;
    EXPECT_FALSE(
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {numeric}}}).isSuccess());

    auto text = validDynamixelReading();
    text["online"] = "true";
    EXPECT_FALSE(
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {text}}}).isSuccess());
}

TEST(SensorReportJson, StillRejectsGenuinelyUnknownFields) {
    // The allowlist is the thing that caught #229 in the first place; widening
    // it by one key must not turn it into a sieve.
    auto reading = validDynamixelReading();
    reading["onlien"] = true; // plausible typo
    EXPECT_FALSE(
        dynamixelSensorReportFromJson({{"creature_id", CREATURE_ID}, {"dynamixel_motors", {reading}}}).isSuccess());
}

} // namespace creatures
