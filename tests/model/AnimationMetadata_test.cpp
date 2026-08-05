
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/AnimationMetadata.h"

namespace creatures {

class AnimationMetadataTest : public ::testing::Test {
  protected:
    AnimationMetadata metadata;

    void SetUp() override {
        metadata.animation_id = "anim123";
        metadata.title = "Test Animation";
        metadata.milliseconds_per_frame = 20;
        metadata.note = "A simple test animation";
        metadata.sound_file = "sound.mp3";
        metadata.number_of_frames = 100;
        metadata.multitrack_audio = true;
    }
};

//    TEST_F(AnimationMetadataTest, Serialization) {
//    nlohmann::json j = metadata;
//    ASSERT_EQ(j["animation_id"], metadata.animation_id);
//    ASSERT_EQ(j["title"], metadata.title);
//    ASSERT_EQ(j["milliseconds_per_frame"], metadata.milliseconds_per_frame);
//    ASSERT_EQ(j["note"], metadata.note);
//    ASSERT_EQ(j["sound_file"], metadata.sound_file);
//    ASSERT_EQ(j["number_of_frames"], metadata.number_of_frames);
//    ASSERT_EQ(j["multitrack_audio"], metadata.multitrack_audio);
//}
//
// TEST_F(AnimationMetadataTest, Deserialization) {
// nlohmann::json j = {
//        {"animation_id", metadata.animation_id},
//        {"title", metadata.title},
//        {"milliseconds_per_frame", metadata.milliseconds_per_frame},
//        {"note", metadata.note},
//        {"sound_file", metadata.sound_file},
//        {"number_of_frames", metadata.number_of_frames},
//        {"multitrack_audio", metadata.multitrack_audio}
//};
//
// AnimationMetadata newMetadata = j.get<AnimationMetadata>();
// ASSERT_EQ(newMetadata.animation_id, metadata.animation_id);
// ASSERT_EQ(newMetadata.title, metadata.title);
// ASSERT_EQ(newMetadata.milliseconds_per_frame, metadata.milliseconds_per_frame);
// ASSERT_EQ(newMetadata.note, metadata.note);
// ASSERT_EQ(newMetadata.sound_file, metadata.sound_file);
// ASSERT_EQ(newMetadata.number_of_frames, metadata.number_of_frames);
// ASSERT_EQ(newMetadata.multitrack_audio, metadata.multitrack_audio);
//}

} // namespace creatures

// ===========================================================================
// Stage provenance round-trips (issues #119, #123)
//
// source_render_choices was persisted by animationToJson and read back by the
// JSON parser, but never added to the DTO — so it was invisible on every HTTP
// response even though the re-render job could see it. Exactly the shape of
// #123, in a field added in the same session.
//
// The lesson these tests encode: a new metadata field has to survive BOTH the
// JSON path and the DTO path. Covering only one is how both bugs happened.
// ===========================================================================

TEST(AnimationMetadataStageProvenanceTest, RenderChoicesSurviveTheDtoRoundTrip) {
    creatures::AnimationMetadata md;
    md.animation_id = "c7471c45-58bf-4a34-8a46-76dca6612d72";
    md.title = "scene";
    md.milliseconds_per_frame = 20;
    md.number_of_frames = 211;

    creatures::CreatureRenderChoice beaky;
    beaky.creature_id = "4754fc0e-1706-11ef-931d-bbb95a696e2e";
    beaky.speech_loop_animation_id = "0282df2c-0000-4000-8000-000000000001";
    beaky.idle_animation_id = "ea849c1f-0000-4000-8000-000000000002";
    beaky.idle_start_offset = 87;
    md.source_render_choices.push_back(beaky);

    const auto dto = creatures::convertToDto(md);
    ASSERT_TRUE(dto->source_render_choices);
    ASSERT_EQ(dto->source_render_choices->size(), 1u);

    const auto back = creatures::convertFromDto(dto);
    ASSERT_EQ(back.source_render_choices.size(), 1u);
    const auto &r = back.source_render_choices[0];
    EXPECT_EQ(r.creature_id, beaky.creature_id);
    EXPECT_EQ(r.speech_loop_animation_id, beaky.speech_loop_animation_id);
    EXPECT_EQ(r.idle_animation_id, beaky.idle_animation_id);
    EXPECT_EQ(r.idle_start_offset, 87u);
}

TEST(AnimationMetadataStageProvenanceTest, StagePointerAndSeedSurviveTheDtoRoundTrip) {
    creatures::AnimationMetadata md;
    md.animation_id = "c7471c45-58bf-4a34-8a46-76dca6612d72";
    md.title = "scene";
    md.milliseconds_per_frame = 20;
    md.number_of_frames = 211;
    md.source_stage_id = "17a3b300-d3d7-40f1-b217-671c7cae5fd0";
    md.source_stage_updated_at = 1785911429306;
    md.render_seed = 1785911444559622805ULL;

    const auto back = creatures::convertFromDto(creatures::convertToDto(md));
    EXPECT_EQ(back.source_stage_id, md.source_stage_id);
    EXPECT_EQ(back.source_stage_updated_at, md.source_stage_updated_at);
    EXPECT_EQ(back.render_seed, md.render_seed);
}

TEST(AnimationMetadataStageProvenanceTest, AnimationWithoutProvenanceOmitsItAllEntirely) {
    creatures::AnimationMetadata md;
    md.animation_id = "c7471c45-58bf-4a34-8a46-76dca6612d72";
    md.title = "plain animation";
    md.milliseconds_per_frame = 20;
    md.number_of_frames = 10;

    const auto dto = creatures::convertToDto(md);
    EXPECT_FALSE(dto->source_stage_id);
    EXPECT_FALSE(dto->render_seed);
    EXPECT_FALSE(dto->source_render_choices);
}
