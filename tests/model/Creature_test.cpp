
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Creature.h"
#include "server/database.h"

namespace creatures {

class CreatureTest : public ::testing::Test {
  protected:
    Creature creature; // Test fixture

    void SetUp() override {
        // Set up your test fixture with known values
        creature.id = "507f1f77bcf86cd799439011";
        creature.name = "BunnyBot";
        creature.channel_offset = 10;
        creature.audio_channel = 5;
    }
};

// Test serialization to JSON
// TEST_F(CreatureTest, SerializesToJsonCorrectly) {
//    nlohmann::json j = creature;
//
//    // Assert that all fields are serialized correctly
//    EXPECT_EQ(j["id"], creature.id);
//    EXPECT_EQ(j["name"], creature.name);
//    EXPECT_EQ(j["channel_offset"], creature.channel_offset);
//    EXPECT_EQ(j["audio_channel"], creature.audio_channel);
//    EXPECT_EQ(j["notes"], creature.notes);
//
//}
//
//// Test deserialization from JSON
// TEST_F(CreatureTest, DeserializesFromJsonCorrectly) {
//     nlohmann::json j = {
//         {"id", creature.id},
//         {"name", creature.name},
//         {"channel_offset", creature.channel_offset},
//         {"audio_channel", creature.audio_channel},
//         {"notes", creature.notes}
//     };
//
//     auto new_creature = j.get<Creature>();
//
//     // Assert that all fields are deserialized correctly
//     EXPECT_EQ(new_creature.id, creature.id);
//     EXPECT_EQ(new_creature.name, creature.name);
//     EXPECT_EQ(new_creature.channel_offset, creature.channel_offset);
//     EXPECT_EQ(new_creature.audio_channel, creature.audio_channel);
//     EXPECT_EQ(new_creature.notes, creature.notes);
//
// }

TEST(CreatureValidationTest, AcceptsIdleAndSpeechLoopLists) {
    nlohmann::json creatureJson = {
        {"id", "4754fc0e-1706-11ef-931d-bbb95a696e2e"},
        {"name", "Test Creature"},
        {"channel_offset", 10},
        {"audio_channel", 1},
        {"mouth_slot", 2},
        {"inputs", {{{"name", "head"}, {"slot", 0}, {"width", 1}, {"joystick_axis", 0}}}},
        {"speech_loop_animation_ids", {"87450ac7-4947-4078-a049-13dd767708df", "4ee4ea7a-029e-4015-ae8f-84247c92dee6"}},
        {"idle_animation_ids", {"0282df2c-ec7f-4869-8a09-43216e3bd715", "ea849c1f-e81e-4877-ad4f-b3af827aedc6"}}};

    auto result = Database::validateCreatureJson(creatureJson);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "validation failed");
    EXPECT_TRUE(result.getValue().value());
}

TEST(CreatureJson, BeakyShapeRoundTripsThroughTheNeutralCodec) {
    const nlohmann::json beaky = {
        {"id", "4754fc0e-1706-11ef-931d-bbb95a696e2e"},
        {"name", "Beaky"},
        {"channel_offset", 1},
        {"audio_channel", 1},
        {"mouth_slot", 4},
        {"inputs",
         {{{"name", "neck_rotate"}, {"slot", 0}, {"width", 1}, {"joystick_axis", 0}},
          {{"name", "stand_rotate"}, {"slot", 1}, {"width", 1}, {"joystick_axis", 1}},
          {{"name", "head_tilt"}, {"slot", 2}, {"width", 1}, {"joystick_axis", 2}},
          {{"name", "head_height"}, {"slot", 3}, {"width", 1}, {"joystick_axis", 3}},
          {{"name", "beak"}, {"slot", 4}, {"width", 1}, {"joystick_axis", 4}},
          {{"name", "body_lean"}, {"slot", 5}, {"width", 1}, {"joystick_axis", 5}},
          {{"name", "chest"}, {"slot", 7}, {"width", 1}, {"joystick_axis", 7}}}},
        {"speech_loop_animation_ids", {"87450ac7-4947-4078-a049-13dd767708df"}},
        {"controller_hardware", {{"firmware_revision", "beaky-2026"}}},
        {"gaze",
         {{"pan",
           {{"input", "neck_rotate"}, {"degrees_at_min", -55}, {"degrees_at_max", 55}, {"listening_amount", 0.4}}},
          {"elevation",
           {{"input", "head_height"}, {"degrees_at_min", -25}, {"degrees_at_max", 25}, {"listening_amount", 0.4}}},
          {"cock",
           {{"input", "head_tilt"}, {"degrees_at_min", -20}, {"degrees_at_max", 20}, {"listening_amount", 0.4}}}}},
    };

    const auto parsed = creatureFromJson(beaky);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    const auto canonical = creatureToJson(parsed.getValue().value());
    EXPECT_EQ(canonical["id"], beaky["id"]);
    EXPECT_EQ(canonical["name"], beaky["name"]);
    EXPECT_EQ(canonical["inputs"], beaky["inputs"]);
    EXPECT_EQ(canonical["speech_loop_animation_ids"], beaky["speech_loop_animation_ids"]);
    const auto roundTrip = creatureFromJson(canonical);
    ASSERT_TRUE(roundTrip.isSuccess()) << roundTrip.getError()->getMessage();
    EXPECT_EQ(roundTrip.getValue()->gaze->pan->input, "neck_rotate");
    EXPECT_FLOAT_EQ(roundTrip.getValue()->gaze->pan->degrees_at_min, -55.0f);
    EXPECT_FALSE(canonical.contains("runtime"));
    EXPECT_FALSE(canonical.contains("controller_hardware"));
}

TEST(CreatureJson, RejectsInvalidAggregateConstraints) {
    const nlohmann::json base = {{"id", "4754fc0e-1706-11ef-931d-bbb95a696e2e"},
                                 {"name", "Beaky"},
                                 {"channel_offset", 1},
                                 {"audio_channel", 1},
                                 {"mouth_slot", 4},
                                 {"inputs", {{{"name", "beak"}, {"slot", 4}, {"width", 1}, {"joystick_axis", 4}}}}};

    auto invalidId = base;
    invalidId["id"] = "beaky";
    EXPECT_FALSE(creatureFromJson(invalidId).isSuccess());

    auto reservedDatabaseId = base;
    reservedDatabaseId["_id"] = "controller-must-not-set-this";
    EXPECT_FALSE(creatureFromJson(reservedDatabaseId).isSuccess());

    auto duplicateName = base;
    duplicateName["inputs"].push_back({{"name", "beak"}, {"slot", 5}, {"width", 1}, {"joystick_axis", 5}});
    EXPECT_FALSE(creatureFromJson(duplicateName).isSuccess());

    auto overlapping = base;
    overlapping["inputs"].push_back({{"name", "head"}, {"slot", 4}, {"width", 1}, {"joystick_axis", 5}});
    EXPECT_FALSE(creatureFromJson(overlapping).isSuccess());

    auto missingMouthInput = base;
    missingMouthInput["mouth_input"] = "head";
    EXPECT_FALSE(creatureFromJson(missingMouthInput).isSuccess());

    auto invalidAudioChannel = base;
    invalidAudioChannel["audio_channel"] = 17;
    EXPECT_FALSE(creatureFromJson(invalidAudioChannel).isSuccess());

    auto overflowingUniverse = base;
    overflowingUniverse["channel_offset"] = 511;
    overflowingUniverse["inputs"][0]["slot"] = 0;
    overflowingUniverse["inputs"][0]["width"] = 2;
    EXPECT_FALSE(creatureFromJson(overflowingUniverse).isSuccess());
}

} // namespace creatures

// ===========================================================================
// Degree-of-freedom references (issue #120)
//
// The slot NUMBER a semantic axis lives at is a per-creature implementation
// detail — the two creature families lay their inputs out differently. What
// matters is the LINK: the mouth reference has to point at the beak.
// ===========================================================================

namespace {

/// Beaky / Crow input layout: beak at slot 5.
creatures::Creature beakyLayoutCreature() {
    creatures::Creature c;
    c.id = "beaky";
    c.name = "Beaky";
    c.mouth_slot = 4;
    c.inputs = {{"head_tilt", 0, 1, 0}, {"head_height", 1, 1, 1}, {"neck_rotate", 2, 1, 2}, {"stand_rotate", 3, 1, 3},
                {"body_lean", 4, 1, 4}, {"beak", 5, 1, 5},        {"chest", 6, 1, 6}};
    return c;
}

/// Mango / Kenny input layout: same names, beak at slot 2.
creatures::Creature mangoLayoutCreature() {
    creatures::Creature c;
    c.id = "mango";
    c.name = "Mango";
    c.mouth_slot = 4;
    c.inputs = {{"stand_rotate", 0, 1, 0}, {"beak", 2, 1, 2},      {"head_tilt", 3, 1, 3}, {"head_height", 4, 1, 4},
                {"neck_rotate", 5, 1, 5},  {"body_lean", 6, 1, 6}, {"chest", 7, 1, 7}};
    return c;
}

} // namespace

TEST(CreatureInputLookupTest, FindsSlotByName) {
    const auto creature = beakyLayoutCreature();
    EXPECT_EQ(creatures::inputSlotByName(creature, "beak").value(), 5);
    EXPECT_EQ(creatures::inputSlotByName(creature, "neck_rotate").value(), 2);
}

TEST(CreatureInputLookupTest, UnknownNameHasNoSlot) {
    const auto creature = beakyLayoutCreature();
    EXPECT_FALSE(creatures::inputSlotByName(creature, "wing_flap").has_value());
}

TEST(CreatureInputLookupTest, SameNameResolvesDifferentlyPerCreature) {
    // The reason semantic references name the axis instead of numbering it.
    EXPECT_EQ(creatures::inputSlotByName(beakyLayoutCreature(), "beak").value(), 5);
    EXPECT_EQ(creatures::inputSlotByName(mangoLayoutCreature(), "beak").value(), 2);
}

TEST(ResolvedMouthSlotTest, FallsBackToTheRawSlotWhenNoInputNamed) {
    const auto creature = beakyLayoutCreature(); // mouth_input unset
    EXPECT_EQ(creatures::resolvedMouthSlot(creature), 4);
}

TEST(ResolvedMouthSlotTest, MouthInputWinsOverTheRawSlot) {
    auto creature = beakyLayoutCreature();
    creature.mouth_input = "beak";
    EXPECT_EQ(creatures::resolvedMouthSlot(creature), 5);
}

TEST(ResolvedMouthSlotTest, TheSameMouthInputWorksAcrossBothLayouts) {
    // One config line, two different birds, correct slot on each. This is the
    // whole point of referencing the degree of freedom by name.
    auto beaky = beakyLayoutCreature();
    beaky.mouth_input = "beak";
    auto mango = mangoLayoutCreature();
    mango.mouth_input = "beak";

    EXPECT_EQ(creatures::resolvedMouthSlot(beaky), 5);
    EXPECT_EQ(creatures::resolvedMouthSlot(mango), 2);
}

TEST(ResolvedMouthSlotTest, UnresolvableMouthInputFallsBackRatherThanGuessing) {
    auto creature = beakyLayoutCreature();
    creature.mouth_input = "wing_flap";
    EXPECT_EQ(creatures::resolvedMouthSlot(creature), 4);
}

TEST(MouthBeakLinkTest, DetectsTheMismatchInTheShippedConfigs) {
    // Both real layouts currently point mouth_slot at something that is not
    // the beak: slot 4 is body_lean on Beaky and head_height on Mango.
    EXPECT_FALSE(creatures::mouthSlotMatchesBeak(beakyLayoutCreature()).value());
    EXPECT_FALSE(creatures::mouthSlotMatchesBeak(mangoLayoutCreature()).value());
}

TEST(MouthBeakLinkTest, IsSatisfiedWhenTheSlotPointsAtTheBeak) {
    auto creature = beakyLayoutCreature();
    creature.mouth_slot = 5; // corrected by number
    EXPECT_TRUE(creatures::mouthSlotMatchesBeak(creature).value());

    auto named = beakyLayoutCreature();
    named.mouth_input = "beak"; // corrected by name
    EXPECT_TRUE(creatures::mouthSlotMatchesBeak(named).value());
}

TEST(MouthBeakLinkTest, TheNumberItselfIsNotWhatMatters) {
    // mouth_slot = 2 is correct on Mango and wrong on Beaky. There is no
    // "right" number; there is only the right link.
    auto mango = mangoLayoutCreature();
    mango.mouth_slot = 2;
    EXPECT_TRUE(creatures::mouthSlotMatchesBeak(mango).value());

    auto beaky = beakyLayoutCreature();
    beaky.mouth_slot = 2;
    EXPECT_FALSE(creatures::mouthSlotMatchesBeak(beaky).value());
}

TEST(MouthBeakLinkTest, CreatureWithNoBeakInputCannotBeChecked) {
    creatures::Creature creature;
    creature.mouth_slot = 0;
    creature.inputs = {{"pan", 0, 1, 0}}; // a light, say — no beak at all
    EXPECT_FALSE(creatures::mouthSlotMatchesBeak(creature).has_value());
}
