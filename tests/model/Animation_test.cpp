
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"

namespace creatures {

class AnimationTest : public ::testing::Test {
  protected:
    Animation animation;

    void SetUp() override {
        animation.id = "anim123";
        animation.metadata.animation_id = "anim123";
        animation.metadata.title = "Dance Party";
        animation.metadata.milliseconds_per_frame = 20;
        animation.metadata.note = "Important notes";
        animation.metadata.sound_file = "song.mp3";
        animation.metadata.number_of_frames = 100;
        animation.metadata.multitrack_audio = true;
        Track trackData = {.id = "frame123",
                           .creature_id = "creature456",
                           .fixture_id = "",
                           .animation_id = "anim123",
                           .frames = {"base64encodedframe1", "base64encodedframe2"}};
        animation.tracks.push_back(trackData);
    }
};

TEST_F(AnimationTest, NeutralJsonSerializesCreatureTrackAndOmitsFixtureId) {
    const auto json = animationToJson(animation);

    const nlohmann::json expected{
        {"id", "anim123"},
        {"metadata",
         {{"animation_id", "anim123"},
          {"title", "Dance Party"},
          {"milliseconds_per_frame", 20},
          {"note", "Important notes"},
          {"sound_file", "song.mp3"},
          {"number_of_frames", 100},
          {"multitrack_audio", true}}},
        {"tracks", nlohmann::json::array({{{"id", "frame123"},
                                           {"creature_id", "creature456"},
                                           {"animation_id", "anim123"},
                                           {"frames", {"base64encodedframe1", "base64encodedframe2"}}}})}};
    EXPECT_EQ(json, expected);
}

TEST_F(AnimationTest, NeutralJsonSerializesFixtureTrackAndOmitsCreatureId) {
    animation.tracks[0].creature_id.clear();
    animation.tracks[0].fixture_id = "fixture789";

    const auto json = animationToJson(animation);

    const nlohmann::json expectedTrack{{"id", "frame123"},
                                       {"fixture_id", "fixture789"},
                                       {"animation_id", "anim123"},
                                       {"frames", {"base64encodedframe1", "base64encodedframe2"}}};
    EXPECT_EQ(json["tracks"], nlohmann::json::array({expectedTrack}));
}

TEST_F(AnimationTest, NeutralJsonOmitsAbsentOptionalMetadata) {
    animation.metadata.note.clear();

    const auto json = animationToJson(animation);
    const auto &metadata = json["metadata"];

    EXPECT_FALSE(metadata.contains("note"));
    EXPECT_FALSE(metadata.contains("source_script_id"));
    EXPECT_FALSE(metadata.contains("source_script_turns"));
    EXPECT_FALSE(metadata.contains("source_stage_id"));
    EXPECT_FALSE(metadata.contains("source_stage_updated_at"));
    EXPECT_FALSE(metadata.contains("render_seed"));
    EXPECT_FALSE(metadata.contains("source_render_choices"));
}

TEST_F(AnimationTest, NeutralJsonIncludesPresentDialogAndStageProvenance) {
    animation.metadata.source_script_id = "script-id";
    animation.metadata.source_script_turns.push_back({"speaker-id", "Hello"});
    animation.metadata.source_stage_id = "stage-id";
    animation.metadata.source_stage_updated_at = 1234;
    animation.metadata.render_seed = 5678;
    animation.metadata.source_render_choices.push_back({"speaker-id", "speech-id", "", 0});

    const auto json = animationToJson(animation);
    const auto &metadata = json["metadata"];

    EXPECT_EQ(metadata["source_script_id"], "script-id");
    EXPECT_EQ(metadata["source_script_turns"],
              nlohmann::json::array({{{"creature_id", "speaker-id"}, {"text", "Hello"}}}));
    EXPECT_EQ(metadata["source_stage_id"], "stage-id");
    EXPECT_EQ(metadata["source_stage_updated_at"], 1234);
    EXPECT_EQ(metadata["render_seed"], 5678);
    ASSERT_EQ(metadata["source_render_choices"].size(), 1u);
    const auto &choice = metadata["source_render_choices"][0];
    EXPECT_EQ(choice["creature_id"], "speaker-id");
    EXPECT_EQ(choice["speech_loop_animation_id"], "speech-id");
    EXPECT_FALSE(choice.contains("idle_animation_id"));
    EXPECT_FALSE(choice.contains("idle_start_offset"));
}

} // namespace creatures
