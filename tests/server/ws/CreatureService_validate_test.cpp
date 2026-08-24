#include "gtest/gtest.h"

#include "../TestGlobals.h"
#include "api/CreatureRequests.h"
#include "api/CreatureResponse.h"
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
    EXPECT_FALSE(result.valid);
    EXPECT_FALSE(result.errorMessages.empty());
}

TEST_F(CreatureServiceValidateTest, RejectsJsonBeyondTheApiNestingLimit) {
    std::string nested = "null";
    for (int index = 0; index < 33; ++index)
        nested = "[" + nested + "]";
    const std::string payload =
        R"json({"id":"4754fc0e-1706-11ef-931d-bbb95a696e2e","name":"Test Creature","channel_offset":0,"audio_channel":1,"mouth_slot":2,"controller_extension":)json" +
        nested + "}";

    auto result = creatures::ws::CreatureService::validateCreatureConfig(payload, nullptr);
    EXPECT_FALSE(result.valid);
    ASSERT_EQ(result.errorMessages.size(), 1);
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
    EXPECT_TRUE(result.valid);
    EXPECT_TRUE(result.missingAnimationIds.empty());
    EXPECT_TRUE(result.mismatchedAnimationIds.empty());
    EXPECT_TRUE(result.errorMessages.empty());
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
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.missingAnimationIds.empty());
    EXPECT_TRUE(result.errorMessages.empty());
    ASSERT_EQ(result.mismatchedAnimationIds.size(), 1);
    auto it = std::find(result.mismatchedAnimationIds.begin(), result.mismatchedAnimationIds.end(),
                        "11111111-1111-4111-8111-111111111111");
    EXPECT_NE(it, result.mismatchedAnimationIds.end());
}

TEST(CreatureApiContract, ValidationResponsePreservesExactWireShape) {
    creatures::api::CreatureConfigValidationResponse response;
    response.valid = false;
    response.creatureId = "4754fc0e-1706-11ef-931d-bbb95a696e2e";
    response.missingAnimationIds = {"11111111-1111-4111-8111-111111111111"};
    response.errorMessages = {"bad mouth slot"};

    EXPECT_EQ(creatures::api::creatureConfigValidationResponseToJson(response),
              nlohmann::json({{"valid", false},
                              {"creature_id", "4754fc0e-1706-11ef-931d-bbb95a696e2e"},
                              {"missing_animation_ids", {"11111111-1111-4111-8111-111111111111"}},
                              {"mismatched_animation_ids", nlohmann::json::array()},
                              {"error_messages", {"bad mouth slot"}}}));
}

TEST(CreatureApiContract, CreatureResponseIncludesNeutralRuntime) {
    creatures::Creature creature;
    creature.id = "4754fc0e-1706-11ef-931d-bbb95a696e2e";
    creature.name = "Test Creature";
    creature.channel_offset = 0;
    creature.audio_channel = 1;
    creature.mouth_slot = 2;

    creatures::runtime::CreatureRuntimeSnapshot runtime;
    runtime.idleEnabled = false;
    runtime.activity = {"disabled", std::nullopt, std::nullopt, "disabled", "start", "update"};
    const auto json = creatures::api::creatureResponseToJson({creature, runtime});

    EXPECT_EQ(json.at("id"), creature.id);
    EXPECT_EQ(json.at("inputs"), nlohmann::json::array());
    EXPECT_TRUE(json.at("mouth_input").is_null());
    EXPECT_TRUE(json.at("speech_loop_animation_ids").is_null());
    EXPECT_TRUE(json.at("idle_animation_ids").is_null());
    EXPECT_TRUE(json.at("gaze").is_null());
    EXPECT_FALSE(json.at("runtime").at("idle_enabled"));
    EXPECT_TRUE(json.at("runtime").at("activity").at("animation_id").is_null());
    EXPECT_TRUE(json.at("runtime").at("bgm_owner").is_null());
}

TEST(CreatureApiContract, IdleToggleRequiresOneBooleanField) {
    const auto valid = creatures::api::idleToggleRequestFromJson({{"enabled", false}});
    ASSERT_TRUE(valid.isSuccess());
    EXPECT_FALSE(valid.getValue()->enabled);

    EXPECT_FALSE(creatures::api::idleToggleRequestFromJson(nlohmann::json::object()).isSuccess());
    EXPECT_FALSE(creatures::api::idleToggleRequestFromJson({{"enabled", 1}}).isSuccess());
    EXPECT_FALSE(creatures::api::idleToggleRequestFromJson({{"enabled", true}, {"surprise", true}}).isSuccess());
}

TEST(CreatureApiContract, RegistrationParsesAndBoundsUniverse) {
    const auto valid =
        creatures::api::registerCreatureRequestFromJson({{"creature_config", "{}"}, {"universe", 63999}});
    ASSERT_TRUE(valid.isSuccess());
    EXPECT_EQ(valid.getValue()->creatureConfig, "{}");
    EXPECT_EQ(valid.getValue()->universe, 63999u);

    EXPECT_FALSE(
        creatures::api::registerCreatureRequestFromJson({{"creature_config", "{}"}, {"universe", 0}}).isSuccess());
    EXPECT_FALSE(
        creatures::api::registerCreatureRequestFromJson({{"creature_config", "{}"}, {"universe", 64000}}).isSuccess());
    EXPECT_FALSE(
        creatures::api::registerCreatureRequestFromJson({{"creature_config", "{}"}, {"universe", 1}, {"extra", true}})
            .isSuccess());
}
