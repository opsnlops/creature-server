#include "gtest/gtest.h"

#include "../TestGlobals.h"
#include "server/database.h"
#include "server/ws/service/CreatureService.h"
#include "util/ObservabilityManager.h"

#include <algorithm>

class CreatureServiceValidateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        creatures::observability = std::make_shared<creatures::ObservabilityManager>();
        creatures::db = std::make_shared<creatures::Database>("");
    }

    void TearDown() override {
        creatures::db.reset();
        creatures::observability.reset();
    }
};

TEST_F(CreatureServiceValidateTest, RejectsInvalidJson) {
    auto result = creatures::ws::CreatureService::validateCreatureConfig("{not-json", nullptr);
    ASSERT_TRUE(result);
    EXPECT_FALSE(*result->valid);
    ASSERT_NE(result->error_messages, nullptr);
    EXPECT_GT(result->error_messages->size(), 0);
}

TEST_F(CreatureServiceValidateTest, RejectsJsonBeyondTheApiNestingLimit) {
    std::string nested = "null";
    for (int index = 0; index < 33; ++index)
        nested = "[" + nested + "]";
    const std::string payload =
        R"json({"id":"4754fc0e-1706-11ef-931d-bbb95a696e2e","name":"Test Creature","channel_offset":0,"audio_channel":1,"mouth_slot":2,"controller_extension":)json" +
        nested + "}";

    auto result = creatures::ws::CreatureService::validateCreatureConfig(payload, nullptr);
    ASSERT_TRUE(result);
    EXPECT_FALSE(*result->valid);
    ASSERT_EQ(result->error_messages->size(), 1);
}

TEST_F(CreatureServiceValidateTest, AcceptsValidCreatureConfig) {
    const std::string payload = R"json(
{
  "id": "4754fc0e-1706-11ef-931d-bbb95a696e2e",
  "name": "Test Creature",
  "channel_offset": 0,
  "audio_channel": 1,
  "mouth_slot": 2
}
)json";
    auto result = creatures::ws::CreatureService::validateCreatureConfig(payload, nullptr);
    ASSERT_TRUE(result);
    EXPECT_TRUE(*result->valid);
    EXPECT_EQ(result->missing_animation_ids->size(), 0);
    EXPECT_EQ(result->mismatched_animation_ids->size(), 0);
    EXPECT_EQ(result->error_messages->size(), 0);
}

TEST_F(CreatureServiceValidateTest, RejectsMismatchedAnimationIds) {
    const std::string payload = R"json(
{
  "id": "4754fc0e-1706-11ef-931d-bbb95a696e2e",
  "name": "Test Creature",
  "channel_offset": 0,
  "audio_channel": 1,
  "mouth_slot": 2,
  "idle_animation_ids": [
    "11111111-1111-4111-8111-111111111111",
    "22222222-2222-4222-8222-222222222222"
  ]
}
)json";
    auto result = creatures::ws::CreatureService::validateCreatureConfig(payload, nullptr);
    ASSERT_TRUE(result);
    EXPECT_FALSE(*result->valid);
    EXPECT_EQ(result->missing_animation_ids->size(), 0);
    EXPECT_EQ(result->error_messages->size(), 0);
    ASSERT_EQ(result->mismatched_animation_ids->size(), 1);
    auto it = std::find(result->mismatched_animation_ids->begin(), result->mismatched_animation_ids->end(),
                        "11111111-1111-4111-8111-111111111111");
    EXPECT_NE(it, result->mismatched_animation_ids->end());
}
