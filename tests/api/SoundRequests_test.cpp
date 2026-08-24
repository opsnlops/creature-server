#include <gtest/gtest.h>

#include "api/SoundRequests.h"

namespace creatures::api {

TEST(SoundRequests, ParsesPlayRequest) {
    const auto result = playSoundRequestFromJson({{"file_name", "scene.wav"}});
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue()->fileName, "scene.wav");
}

TEST(SoundRequests, RejectsMissingWrongTypeAndUnknownPlayFields) {
    EXPECT_FALSE(playSoundRequestFromJson(nlohmann::json::object()).isSuccess());
    EXPECT_FALSE(playSoundRequestFromJson({{"file_name", 7}}).isSuccess());
    EXPECT_FALSE(playSoundRequestFromJson({{"file_name", "../scene.wav"}}).isSuccess());
    EXPECT_FALSE(playSoundRequestFromJson({{"file_name", "scene.wav"}, {"extra", true}}).isSuccess());
}

TEST(SoundRequests, ParsesLipSyncDefaultsAndOverwrite) {
    const auto defaultResult = generateLipSyncRequestFromJson({{"sound_file", "scene.wav"}});
    ASSERT_TRUE(defaultResult.isSuccess());
    EXPECT_FALSE(defaultResult.getValue()->allowOverwrite);

    const auto overwriteResult =
        generateLipSyncRequestFromJson({{"sound_file", "scene.wav"}, {"allow_overwrite", true}});
    ASSERT_TRUE(overwriteResult.isSuccess());
    EXPECT_TRUE(overwriteResult.getValue()->allowOverwrite);
}

TEST(SoundRequests, RejectsInvalidLipSyncShape) {
    EXPECT_FALSE(generateLipSyncRequestFromJson({{"sound_file", "scene.wav"}, {"allow_overwrite", 1}}).isSuccess());
    EXPECT_FALSE(generateLipSyncRequestFromJson({{"sound_file", "scene.wav"}, {"unknown", false}}).isSuccess());
    EXPECT_FALSE(generateLipSyncRequestFromJson({{"sound_file", std::string(256, 'a')}}).isSuccess());
    EXPECT_FALSE(generateLipSyncRequestFromJson({{"sound_file", "folder/scene.wav"}}).isSuccess());
}

} // namespace creatures::api
