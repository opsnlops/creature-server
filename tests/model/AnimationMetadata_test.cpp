
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

    TEST_F(AnimationMetadataTest, Serialization) {
    nlohmann::json j = metadata;
    ASSERT_EQ(j["animation_id"], metadata.animation_id);
    ASSERT_EQ(j["title"], metadata.title);
    ASSERT_EQ(j["milliseconds_per_frame"], metadata.milliseconds_per_frame);
    ASSERT_EQ(j["note"], metadata.note);
    ASSERT_EQ(j["sound_file"], metadata.sound_file);
    ASSERT_EQ(j["number_of_frames"], metadata.number_of_frames);
    ASSERT_EQ(j["multitrack_audio"], metadata.multitrack_audio);
}

TEST_F(AnimationMetadataTest, Deserialization) {
nlohmann::json j = {
        {"animation_id", metadata.animation_id},
        {"title", metadata.title},
        {"milliseconds_per_frame", metadata.milliseconds_per_frame},
        {"note", metadata.note},
        {"sound_file", metadata.sound_file},
        {"number_of_frames", metadata.number_of_frames},
        {"multitrack_audio", metadata.multitrack_audio}
};

AnimationMetadata newMetadata = j.get<AnimationMetadata>();
ASSERT_EQ(newMetadata.animation_id, metadata.animation_id);
ASSERT_EQ(newMetadata.title, metadata.title);
ASSERT_EQ(newMetadata.milliseconds_per_frame, metadata.milliseconds_per_frame);
ASSERT_EQ(newMetadata.note, metadata.note);
ASSERT_EQ(newMetadata.sound_file, metadata.sound_file);
ASSERT_EQ(newMetadata.number_of_frames, metadata.number_of_frames);
ASSERT_EQ(newMetadata.multitrack_audio, metadata.multitrack_audio);
}

} // namespace creatures
