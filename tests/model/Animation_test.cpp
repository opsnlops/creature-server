
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>


#include "model/Animation.h"
#include "model/AnimationMetadata.h"


namespace creatures {

    class AnimationTest : public ::testing::Test {
    protected:
        Animation animation;

        void SetUp() override {
            // Setup some initial values
            animation.id = "anim123";
            animation.metadata = {
                    "anim123",
                    "Dance Party",
                    20,
                    "Important notes",
                    "song.mp3",
                    100,
                    true
            };
            Track trackData = {
                    "frame123",
                    "creature456",
                    "anim123",
                    {"base64encodedframe1", "base64encodedframe2"}
            };
           animation.tracks.push_back(trackData);
        }
    };

//    TEST_F(AnimationTest, Serialization) {
//        nlohmann::json j = animation;
//        ASSERT_EQ(j["id"], animation.id);
//        ASSERT_EQ(j["metadata"]["title"], animation.metadata.title);
//        ASSERT_EQ(j["frames"][0]["creature_id"], animation.tracks[0].creature_id);
//    }

//    TEST_F(AnimationTest, Deserialization) {
//        nlohmann::json j = {
//                {"id", animation.id},
//                {"metadata", {
//                               {"animation_id", animation.metadata.animation_id},
//                               {"title", animation.metadata.title},
//                               {"milliseconds_per_frame", animation.metadata.milliseconds_per_frame},
//                               {"note", animation.metadata.note},
//                               {"sound_file", animation.metadata.sound_file},
//                               {"number_of_frames", animation.metadata.number_of_frames},
//                               {"multitrack_audio", animation.metadata.multitrack_audio}
//                       }},
//                {"frames", {{
//                                {"id", animation.tracks[0].id},
//                                                {"creature_id", animation.tracks[0].creature_id},
//                                       {"animation_id", animation.tracks[0].animation_id},
//                                       {"frames", animation.tracks[0].frames}
//                               }}}
//        };
//
//        Animation new_animation = j.get<Animation>();
//        ASSERT_EQ(new_animation.id, animation.id);
//        ASSERT_EQ(new_animation.metadata.title, animation.metadata.title);
//        ASSERT_EQ(new_animation.tracks[0].creature_id, animation.tracks[0].creature_id);
//    }

} // namespace creatures
