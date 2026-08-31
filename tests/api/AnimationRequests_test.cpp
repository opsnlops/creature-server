#include <gtest/gtest.h>

#include "api/AnimationRequests.h"
#include "api/JobResponses.h"

namespace creatures::api {
namespace {

constexpr const char *ANIMATION_ID = "00000000-0000-4000-8000-000000000001";
constexpr const char *CREATURE_ID = "00000000-0000-4000-8000-000000000002";

TEST(AnimationRequests, ParsesPlayRequestWithLegacyCamelCaseResumeField) {
    const auto defaultResult = playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 42}});
    ASSERT_TRUE(defaultResult.isSuccess()) << defaultResult.getError()->getMessage();
    EXPECT_EQ(defaultResult.getValue()->animationId, ANIMATION_ID);
    EXPECT_EQ(defaultResult.getValue()->universe, 42U);
    EXPECT_FALSE(defaultResult.getValue()->resumePlaylist);

    const auto explicitResult =
        playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 42}, {"resumePlaylist", true}});
    ASSERT_TRUE(explicitResult.isSuccess()) << explicitResult.getError()->getMessage();
    EXPECT_TRUE(explicitResult.getValue()->resumePlaylist);
}

TEST(AnimationRequests, RejectsInvalidPlayFields) {
    EXPECT_FALSE(playAnimationRequestFromJson({{"animation_id", "not-a-uuid"}, {"universe", 1}}).isSuccess());
    EXPECT_FALSE(playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 0}}).isSuccess());
    EXPECT_FALSE(playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 64000}}).isSuccess());
    EXPECT_FALSE(
        playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 1}, {"resumePlaylist", nullptr}})
            .isSuccess());
    EXPECT_FALSE(
        playAnimationRequestFromJson({{"animation_id", ANIMATION_ID}, {"universe", 1}, {"resume_playlist", true}})
            .isSuccess());
}

TEST(AnimationRequests, ParsesAndBoundsAdHocSpeechRequest) {
    const auto result = createAdHocAnimationRequestFromJson({{"creature_id", CREATURE_ID}, {"text", "Hello there"}});
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->creatureId, CREATURE_ID);
    EXPECT_EQ(result.getValue()->text, "Hello there");
    EXPECT_TRUE(result.getValue()->resumePlaylist);

    EXPECT_FALSE(createAdHocAnimationRequestFromJson(
                     {{"creature_id", CREATURE_ID}, {"text", std::string(MAX_AD_HOC_SPEECH_TEXT_BYTES + 1, 'x')}})
                     .isSuccess());
    EXPECT_FALSE(
        createAdHocAnimationRequestFromJson({{"creature_id", CREATURE_ID}, {"text", "Hello"}, {"unexpected", true}})
            .isSuccess());
}

TEST(AnimationRequests, ParsesTriggerAndLipSyncRequests) {
    const auto trigger = triggerAdHocAnimationRequestFromJson({{"animation_id", ANIMATION_ID}});
    ASSERT_TRUE(trigger.isSuccess()) << trigger.getError()->getMessage();
    EXPECT_TRUE(trigger.getValue()->resumePlaylist);

    const auto lipSync = regenerateAnimationLipSyncRequestFromJson({{"animation_id", ANIMATION_ID}});
    ASSERT_TRUE(lipSync.isSuccess()) << lipSync.getError()->getMessage();
    EXPECT_EQ(lipSync.getValue()->animationId, ANIMATION_ID);

    EXPECT_FALSE(
        regenerateAnimationLipSyncRequestFromJson({{"animation_id", ANIMATION_ID}, {"extra", "field"}}).isSuccess());
}

TEST(JobResponses, PreservesJobCreatedWireShape) {
    const JobCreatedResponse response{ANIMATION_ID, "animation-lip-sync", "Queued"};

    EXPECT_EQ(jobCreatedResponseToJson(response),
              (nlohmann::json{{"job_id", ANIMATION_ID}, {"job_type", "animation-lip-sync"}, {"message", "Queued"}}));
}

} // namespace
} // namespace creatures::api
