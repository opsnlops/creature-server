#include <cstdint>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "server/voice/DialogAnimation.h"

using creatures::voice::buildNeutralFrame;

// Use the actual Beaky creature doc (truncated to the fields the neutral
// frame builder reads). Beaky's expected neutral frame is well-defined:
//
//   slot 0 neck_rotate    motor "center"            → 128
//   slot 1 stand_rotate   no matching motor         → 128 (default)
//   slot 2 head_tilt      no matching motor         → 128 (default)
//   slot 3 head_height    no matching motor         → 128 (default)
//   slot 4 beak           motor "min", not inverted → 0   (= mouth closed)
//   slot 5 body_lean      motor "min", INVERTED     → 255 (inverted min)
//   slot 6 (unused)                                 → 0
//   slot 7 chest          no matching motor         → 128 (default)
//
// This is the contract documented in the design memory; the test pins it
// so a future regression in the resolution rule will fail fast here.

namespace {

nlohmann::json beakyCreatureJson() {
    return nlohmann::json::parse(R"json({
      "inputs": [
        {"name": "head_tilt",    "slot": 2, "width": 1},
        {"name": "head_height",  "slot": 3, "width": 1},
        {"name": "neck_rotate",  "slot": 0, "width": 1},
        {"name": "stand_rotate", "slot": 1, "width": 1},
        {"name": "body_lean",    "slot": 5, "width": 1},
        {"name": "beak",         "slot": 4, "width": 1},
        {"name": "chest",        "slot": 7, "width": 1}
      ],
      "motors": [
        {"id": "neck_left",   "inverted": true,  "default_position": "center"},
        {"id": "neck_right",  "inverted": false, "default_position": "center"},
        {"id": "neck_rotate", "inverted": true,  "default_position": "center"},
        {"id": "body_lean",   "inverted": true,  "default_position": "min"},
        {"id": "beak",        "inverted": false, "default_position": "min"}
      ],
      "mouth_slot": 4
    })json");
}

} // namespace

TEST(BuildNeutralFrame, BeakyExpectedDmxValues) {
    auto result = buildNeutralFrame(beakyCreatureJson(), 8);
    ASSERT_TRUE(result.isSuccess());
    const auto frame = result.getValue().value();
    ASSERT_EQ(frame.size(), 8u);

    // The whole expected frame at once — explicit for documentation.
    EXPECT_EQ(frame[0], 128) << "neck_rotate (center) → 128";
    EXPECT_EQ(frame[1], 128) << "stand_rotate (no matching motor) → 128";
    EXPECT_EQ(frame[2], 128) << "head_tilt (no matching motor) → 128";
    EXPECT_EQ(frame[3], 128) << "head_height (no matching motor) → 128";
    EXPECT_EQ(frame[4], 0) << "beak (default min, not inverted) → 0 (mouth closed)";
    EXPECT_EQ(frame[5], 255) << "body_lean (default min, INVERTED) → 255";
    EXPECT_EQ(frame[6], 0) << "slot 6 has no input → stays at zero-init";
    EXPECT_EQ(frame[7], 128) << "chest (no matching motor) → 128";
}

TEST(BuildNeutralFrame, RejectsMissingInputs) {
    const auto j = nlohmann::json::parse(R"json({"motors": []})json");
    auto result = buildNeutralFrame(j, 8);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(BuildNeutralFrame, RejectsZeroFrameWidth) {
    auto result = buildNeutralFrame(beakyCreatureJson(), 0);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(BuildNeutralFrame, SkipsInputsPastFrameWidth) {
    // Regression: live e2e (2026-05-30) caught Beaky's `chest` input at slot
    // 7 against a base anim track only 6 bytes wide. The neutral builder
    // must warn + skip rather than hard-fail — those bytes stay zero, the
    // controller ignores them since the slot is outside its wire payload.
    // Mirrors the mouth_slot bounds check at StreamingAdHocSession.cpp:344.
    auto result = buildNeutralFrame(beakyCreatureJson(), 6);
    ASSERT_TRUE(result.isSuccess());
    const auto frame = result.getValue().value();
    ASSERT_EQ(frame.size(), 6u);
    // In-range inputs still get their neutral values.
    EXPECT_EQ(frame[0], 128) << "neck_rotate still in-range";
    EXPECT_EQ(frame[4], 0) << "beak still in-range";
    EXPECT_EQ(frame[5], 255) << "body_lean still in-range (inverted min)";
    // chest (slot 7) doesn't exist in this frame — silently skipped.
}

TEST(BuildNeutralFrame, InvertedMaxResolvesToZero) {
    // Inverted motor with default_position "max" → DMX 0. Pin the rule.
    const auto j = nlohmann::json::parse(R"json({
      "inputs":  [{"name": "x", "slot": 0, "width": 1}],
      "motors":  [{"id": "x", "inverted": true, "default_position": "max"}]
    })json");
    auto result = buildNeutralFrame(j, 1);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value()[0], 0);
}

TEST(BuildNeutralFrame, NonInvertedMaxResolvesToFullRange) {
    const auto j = nlohmann::json::parse(R"json({
      "inputs":  [{"name": "x", "slot": 0, "width": 1}],
      "motors":  [{"id": "x", "inverted": false, "default_position": "max"}]
    })json");
    auto result = buildNeutralFrame(j, 1);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value()[0], 255);
}

TEST(BuildNeutralFrame, InvertedFlagDoesNotChangeCenter) {
    // "center" is always 128 regardless of inversion — only min/max swap.
    const auto j = nlohmann::json::parse(R"json({
      "inputs":  [{"name": "x", "slot": 0, "width": 1}],
      "motors":  [{"id": "x", "inverted": true, "default_position": "center"}]
    })json");
    auto result = buildNeutralFrame(j, 1);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value()[0], 128);
}

TEST(BuildNeutralFrame, MissingMotorsArrayDefaultsAllToCenter) {
    // motors[] absent or empty → every input falls through to 128.
    const auto j = nlohmann::json::parse(R"json({
      "inputs": [
        {"name": "a", "slot": 0, "width": 1},
        {"name": "b", "slot": 1, "width": 1}
      ]
    })json");
    auto result = buildNeutralFrame(j, 2);
    ASSERT_TRUE(result.isSuccess());
    const auto frame = result.getValue().value();
    EXPECT_EQ(frame[0], 128);
    EXPECT_EQ(frame[1], 128);
}

TEST(BuildNeutralFrame, MultiByteInputWritesValueAcrossWidth) {
    // A width-2 input gets the neutral byte written to both slots.
    const auto j = nlohmann::json::parse(R"json({
      "inputs":  [{"name": "x", "slot": 0, "width": 2}],
      "motors":  [{"id": "x", "inverted": false, "default_position": "min"}]
    })json");
    auto result = buildNeutralFrame(j, 4);
    ASSERT_TRUE(result.isSuccess());
    const auto frame = result.getValue().value();
    EXPECT_EQ(frame[0], 0);
    EXPECT_EQ(frame[1], 0);
    EXPECT_EQ(frame[2], 0) << "outside the input → zero-init";
    EXPECT_EQ(frame[3], 0);
}
