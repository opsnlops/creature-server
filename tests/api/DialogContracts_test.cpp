#include <gtest/gtest.h>

#include "api/DialogContracts.h"

namespace creatures::api {
namespace {

constexpr const char *CREATURE_ID = "00000000-0000-4000-8000-000000000001";
constexpr const char *SCRIPT_ID = "00000000-0000-4000-8000-000000000002";
constexpr const char *GENERATION_ID = "00000000-0000-4000-8000-000000000003";

TEST(DialogContracts, ParsesLegacyNullOptionalsInQueuedDialogRequest) {
    const nlohmann::json json = {{"turns", nullptr},        {"script_id", SCRIPT_ID}, {"persistence", "permanent"},
                                 {"autoplay", nullptr},     {"title", nullptr},       {"stage_id", nullptr},
                                 {"generation_id", nullptr}};

    const auto result = dialogRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->scriptId, SCRIPT_ID);
    EXPECT_FALSE(result.getValue()->autoplay);
    EXPECT_FALSE(result.getValue()->title.has_value());
}

TEST(DialogContracts, RejectsUnknownAndMalformedTurnFields) {
    const nlohmann::json json = {
        {"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}, {"unexpected", true}}}},
        {"script_id", nullptr},
        {"persistence", "adhoc"},
    };

    const auto result = dialogRequestFromJson(json);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_NE(result.getError()->getMessage().find("unknown field"), std::string::npos);
}

TEST(DialogContracts, ParsesAndBoundsDialogMusicRequest) {
    const nlohmann::json json = {{"script_id", SCRIPT_ID},
                                 {"dialog_cache_key", std::string(64, 'a')},
                                 {"dialog_generation_id", GENERATION_ID},
                                 {"prompt", "quiet instrumental underscore"},
                                 {"duration_extension_ms", 60001},
                                 {"generation_mode", "track"}};

    const auto result = dialogMusicRequestFromJson(json);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_NE(result.getError()->getMessage().find("between 0 and 60000"), std::string::npos);
}

TEST(DialogContracts, SerializesPreviewMetadataWithStableWireKeys) {
    DialogPreviewMetaResponse response;
    response.cacheKey = std::string(64, 'b');
    response.generationId = GENERATION_ID;
    response.cached = true;
    response.audioUrl = "/audio.wav";
    response.audioFormat = "pcm_48000";
    response.sampleRate = 48000;
    response.durationSeconds = 1.25;
    response.voiceSegments.push_back({"voice-1", 1, 2, 3});
    response.forcedAlignmentWords.push_back({"hello", 0.0, 0.5});
    response.forcedAlignmentLoss = 0.02;

    const auto json = dialogPreviewMetaResponseToJson(response);

    EXPECT_EQ(json.at("generation_id"), GENERATION_ID);
    EXPECT_EQ(json.at("voice_segments").at(0).at("dialog_input_index"), 3);
    EXPECT_EQ(json.at("forced_alignment_words").at(0).at("text"), "hello");
    EXPECT_TRUE(json.at("forced_alignment_chars").empty());
}

TEST(DialogContracts, RejectsUnsafePreviewGenerationIdAndCreatureId) {
    DialogPreviewRequest request{{{CREATURE_ID, "hello"}}, std::string("../escaped"), false, std::nullopt};
    EXPECT_FALSE(validateDialogPreviewRequest(request).isSuccess());

    request.generationId = GENERATION_ID;
    request.turns.front().creatureId = "not-a-uuid";
    EXPECT_FALSE(validateDialogPreviewRequest(request).isSuccess());
}

} // namespace
} // namespace creatures::api
