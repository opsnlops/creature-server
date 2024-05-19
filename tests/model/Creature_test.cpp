
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Creature.h"

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
//TEST_F(CreatureTest, SerializesToJsonCorrectly) {
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
//TEST_F(CreatureTest, DeserializesFromJsonCorrectly) {
//    nlohmann::json j = {
//        {"id", creature.id},
//        {"name", creature.name},
//        {"channel_offset", creature.channel_offset},
//        {"audio_channel", creature.audio_channel},
//        {"notes", creature.notes}
//    };
//
//    auto new_creature = j.get<Creature>();
//
//    // Assert that all fields are deserialized correctly
//    EXPECT_EQ(new_creature.id, creature.id);
//    EXPECT_EQ(new_creature.name, creature.name);
//    EXPECT_EQ(new_creature.channel_offset, creature.channel_offset);
//    EXPECT_EQ(new_creature.audio_channel, creature.audio_channel);
//    EXPECT_EQ(new_creature.notes, creature.notes);
//
//}

} // namespace creatures
