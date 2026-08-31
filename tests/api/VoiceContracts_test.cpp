#include <gtest/gtest.h>

#include "api/VoiceContracts.h"

namespace creatures::api {
namespace {

constexpr const char *CREATURE_ID = "00000000-0000-4000-8000-000000000001";

TEST(VoiceContracts, ParsesQueuedRequestWithAbsentTitle) {
    const auto result =
        makeSoundFileRequestFromJson(nlohmann::json{{"creature_id", CREATURE_ID}, {"text", "Hello there"}});

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->creatureId, CREATURE_ID);
    EXPECT_FALSE(result.getValue()->title.has_value());
}

TEST(VoiceContracts, RejectsNonUuidAndOversizedText) {
    auto invalidId = makeSoundFileRequestFromJson(nlohmann::json{{"creature_id", "not-a-uuid"}, {"text", "Hello"}});
    ASSERT_FALSE(invalidId.isSuccess());

    auto oversized = makeSoundFileRequestFromJson(
        nlohmann::json{{"creature_id", CREATURE_ID}, {"text", std::string(MAX_VOICE_TEXT_BYTES + 1, 'x')}});
    ASSERT_FALSE(oversized.isSuccess());
}

TEST(VoiceContracts, PreservesCreatureSpeechResponseShape) {
    voice::CreatureSpeechResponse response{true, "speech.wav", "speech.txt", 1234};

    const auto json = creatureSpeechResponseToJson(response);

    EXPECT_EQ(json, (nlohmann::json{{"success", true},
                                    {"sound_file_name", "speech.wav"},
                                    {"transcript_file_name", "speech.txt"},
                                    {"sound_file_size", 1234}}));
}

} // namespace
} // namespace creatures::api
