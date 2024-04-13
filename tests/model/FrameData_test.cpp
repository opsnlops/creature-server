
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/FrameData.h"

namespace creatures {

    class FrameDataTest : public ::testing::Test {
    protected:
        FrameData frameData;

        void SetUp() override {
            frameData.id = "frame123";
            frameData.creature_id = "creature456";
            frameData.animation_id = "anim123";
            frameData.frames = {"base64frame1==", "base64frame2=="};
        }
    };

    TEST_F(FrameDataTest, Serialization) {
        nlohmann::json j = frameData;
        ASSERT_EQ(j["id"], frameData.id);
        ASSERT_EQ(j["creature_id"], frameData.creature_id);
        ASSERT_EQ(j["animation_id"], frameData.animation_id);
        ASSERT_EQ(j["frames"][0], frameData.frames[0]);
        ASSERT_EQ(j["frames"][1], frameData.frames[1]);
    }

    TEST_F(FrameDataTest, Deserialization) {
        nlohmann::json j = {
                {"id", frameData.id},
                {"creature_id", frameData.creature_id},
                {"animation_id", frameData.animation_id},
                {"frames", frameData.frames}
        };

        FrameData newFrameData = j.get<FrameData>();
        ASSERT_EQ(newFrameData.id, frameData.id);
        ASSERT_EQ(newFrameData.creature_id, frameData.creature_id);
        ASSERT_EQ(newFrameData.animation_id, frameData.animation_id);
        ASSERT_EQ(newFrameData.frames[0], frameData.frames[0]);
        ASSERT_EQ(newFrameData.frames[1], frameData.frames[1]);
    }

} // namespace creatures
