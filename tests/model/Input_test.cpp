#include <limits>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Input.h"
#include "server/ws/dto/InputDto.h"

namespace creatures {

namespace {

nlohmann::json validInputJson() { return {{"name", "neck_rotate"}, {"slot", 2}, {"width", 1}, {"joystick_axis", 3}}; }

void expectInvalid(const nlohmann::json &json, const std::string &messagePart) {
    const auto result = inputFromJson(json, "creature.inputs[4]");
    ASSERT_FALSE(result.isSuccess());
    ASSERT_TRUE(result.getError().has_value());
    EXPECT_NE(result.getError()->getMessage().find(messagePart), std::string::npos) << result.getError()->getMessage();
}

} // namespace

TEST(InputJson, SerializesExactCanonicalShape) {
    const Input input{"neck_rotate", 2, 1, 3};
    EXPECT_EQ(inputToJson(input), validInputJson());
}

TEST(InputJson, ParsesAllIntegerTypeBoundaries) {
    auto json = validInputJson();
    json["slot"] = std::numeric_limits<uint16_t>::max();
    json["width"] = std::numeric_limits<uint8_t>::max();
    json["joystick_axis"] = std::numeric_limits<uint8_t>::max();

    const auto result = inputFromJson(json);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value(),
              (Input{"neck_rotate", std::numeric_limits<uint16_t>::max(), std::numeric_limits<uint8_t>::max(),
                     std::numeric_limits<uint8_t>::max()}));
}

TEST(InputJson, RoundTripsThroughNeutralCodec) {
    const Input input{"head_height", 17, 2, 6};
    const auto result = inputFromJson(inputToJson(input));
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value(), input);
}

TEST(InputJson, RejectsMissingNullAndWrongTypeFields) {
    expectInvalid(nlohmann::json::array(), "creature.inputs[4] must be an object");

    for (const auto *field : {"name", "slot", "width", "joystick_axis"}) {
        auto missing = validInputJson();
        missing.erase(field);
        expectInvalid(missing, std::string(".") + field + " is required");

        auto nullValue = validInputJson();
        nullValue[field] = nullptr;
        expectInvalid(nullValue, std::string(".") + field + " must");
    }

    auto wrongName = validInputJson();
    wrongName["name"] = 12;
    expectInvalid(wrongName, ".name must be a string");

    for (const auto *field : {"slot", "width", "joystick_axis"}) {
        auto wrongInteger = validInputJson();
        wrongInteger[field] = "1";
        expectInvalid(wrongInteger, std::string(".") + field + " must be an integer");

        auto booleanValue = validInputJson();
        booleanValue[field] = true;
        expectInvalid(booleanValue, std::string(".") + field + " must be an integer");

        auto fractionalValue = validInputJson();
        fractionalValue[field] = 1.5;
        expectInvalid(fractionalValue, std::string(".") + field + " must be an integer");
    }
}

TEST(InputJson, RejectsInvalidRangesAndUnknownFields) {
    auto emptyName = validInputJson();
    emptyName["name"] = "";
    expectInvalid(emptyName, ".name must not be empty");

    auto longName = validInputJson();
    longName["name"] = std::string(MAX_INPUT_NAME_BYTES + 1, 'n');
    expectInvalid(longName, ".name is 129 bytes; maximum is 128");

    auto negativeSlot = validInputJson();
    negativeSlot["slot"] = -1;
    expectInvalid(negativeSlot, ".slot must not be negative");

    auto slotOverflow = validInputJson();
    slotOverflow["slot"] = static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()) + 1;
    expectInvalid(slotOverflow, ".slot exceeds maximum 65535");

    auto zeroWidth = validInputJson();
    zeroWidth["width"] = 0;
    expectInvalid(zeroWidth, ".width must be greater than zero");

    auto widthOverflow = validInputJson();
    widthOverflow["width"] = 256;
    expectInvalid(widthOverflow, ".width exceeds maximum 255");

    auto joystickOverflow = validInputJson();
    joystickOverflow["joystick_axis"] = 256;
    expectInvalid(joystickOverflow, ".joystick_axis exceeds maximum 255");

    auto unknown = validInputJson();
    unknown["future_field"] = true;
    expectInvalid(unknown, "contains unknown field 'future_field'");
}

TEST(InputLegacyDto, AdaptsWithoutChangingTheNeutralModel) {
    const Input input{"beak", 5, 1, 5};
    const auto dto = convertToDto(input);

    ASSERT_TRUE(dto);
    EXPECT_EQ(std::string(dto->name), input.name);
    EXPECT_EQ(*dto->slot, input.slot);
    EXPECT_EQ(*dto->width, input.width);
    EXPECT_EQ(*dto->joystick_axis, input.joystick_axis);
    EXPECT_EQ(convertFromDto(dto), input);
}

} // namespace creatures
