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

TEST(DialogContracts, RejectsMalformedDialogRequestIdentifiers) {
    nlohmann::json json = {{"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}}}},
                           {"persistence", "adhoc"},
                           {"stage_id", "not-a-uuid"}};

    EXPECT_FALSE(dialogRequestFromJson(json).isSuccess());

    json["stage_id"] = SCRIPT_ID;
    json["generation_id"] = "not-a-uuid";
    EXPECT_FALSE(dialogRequestFromJson(json).isSuccess());
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

TEST(DialogContracts, ValidatesDialogMusicIdentifiersAndMode) {
    nlohmann::json json = {{"script_id", SCRIPT_ID},
                           {"dialog_cache_key", std::string(64, 'a')},
                           {"dialog_generation_id", GENERATION_ID},
                           {"prompt", "quiet instrumental underscore"},
                           {"generation_mode", "track"}};

    ASSERT_TRUE(dialogMusicRequestFromJson(json).isSuccess());

    json["dialog_cache_key"] = std::string(64, 'A');
    EXPECT_FALSE(dialogMusicRequestFromJson(json).isSuccess());
    json["dialog_cache_key"] = std::string(64, 'a');
    json["generation_mode"] = "surprise";
    EXPECT_FALSE(dialogMusicRequestFromJson(json).isSuccess());
}

TEST(DialogContracts, ParsesStrictAcceptVoiceTakeRequest) {
    const nlohmann::json json = {
        {"script_id", SCRIPT_ID}, {"generation_id", GENERATION_ID}, {"dialog_cache_key", std::string(64, 'a')}};

    const auto result = acceptVoiceTakeRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->scriptId, SCRIPT_ID);
    EXPECT_EQ(result.getValue()->generationId, GENERATION_ID);
    EXPECT_EQ(result.getValue()->dialogCacheKey, std::string(64, 'a'));

    auto unknown = json;
    unknown["unexpected"] = true;
    EXPECT_FALSE(acceptVoiceTakeRequestFromJson(unknown).isSuccess());
}

TEST(DialogContracts, PreviewLookupUsesTheSameStrictTurnContract) {
    const nlohmann::json json = {
        {"turns", {{{"creature_id", CREATURE_ID}, {"text", "hello"}}}},
    };

    const auto result = dialogPreviewLookupRequestFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    ASSERT_EQ(result.getValue()->size(), 1);
    EXPECT_EQ(result.getValue()->front().creatureId, CREATURE_ID);

    auto unknown = json;
    unknown["regenerate"] = true;
    EXPECT_FALSE(dialogPreviewLookupRequestFromJson(unknown).isSuccess());
}

TEST(DialogContracts, SerializesCanonicalQueuedRequests) {
    DialogRequest dialog{{{CREATURE_ID, "hello"}}, std::nullopt, "adhoc", true, "A scene", std::nullopt, GENERATION_ID};
    const auto dialogJson = dialogRequestToJson(dialog);
    EXPECT_EQ(dialogJson.at("turns").at(0).at("creature_id"), CREATURE_ID);
    EXPECT_EQ(dialogJson.at("generation_id"), GENERATION_ID);
    EXPECT_FALSE(dialogJson.contains("script_id"));

    DialogPreviewRequest preview{{{CREATURE_ID, "hello"}}, GENERATION_ID, false, std::nullopt};
    const auto previewJson = dialogPreviewRequestToJson(preview);
    EXPECT_EQ(previewJson.at("generation_id"), GENERATION_ID);
    EXPECT_FALSE(previewJson.contains("title"));
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

TEST(DialogContracts, SerializesLookupAndValidationResponses) {
    DialogPreviewLookupResponse lookup{std::string(64, 'c'), {{GENERATION_ID, "2026-08-30T12:00:00Z"}}, GENERATION_ID};
    const auto lookupJson = dialogPreviewLookupResponseToJson(lookup);
    EXPECT_EQ(lookupJson.at("generations").at(0).at("generation_id"), GENERATION_ID);
    EXPECT_EQ(lookupJson.at("latest_generation_id"), GENERATION_ID);

    DialogScriptValidationResponse validation;
    validation.valid = false;
    validation.turnCount = 2;
    validation.errorMessages = {"bad turn"};
    const auto validationJson = dialogScriptValidationResponseToJson(validation);
    EXPECT_FALSE(validationJson.at("valid"));
    EXPECT_FALSE(validationJson.contains("script_id"));
    EXPECT_EQ(validationJson.at("error_messages").at(0), "bad turn");
}

} // namespace
} // namespace creatures::api
