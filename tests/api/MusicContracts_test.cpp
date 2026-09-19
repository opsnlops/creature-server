#include <string>

#include <gtest/gtest.h>

#include "api/MusicContracts.h"

namespace creatures::api {
namespace {

constexpr const char *PIECE_ID = "00000000-0000-4000-8000-000000000010";
constexpr const char *VERSION_ID = "00000000-0000-4000-8000-000000000001";

nlohmann::json section(const char *text, int64_t durationMs) {
    return {{"text", text}, {"duration_ms", durationMs}, {"positive_styles", {"strings"}}};
}

std::string messageOf(const nlohmann::json &json) {
    const auto result = musicGenerateRequestFromJson(json);
    return result.isSuccess() ? std::string("<accepted>") : result.getError()->getMessage();
}

} // namespace

TEST(MusicContracts, PromptModeRequiresAnExplicitLength) {
    nlohmann::json json = {{"prompt", "playful chamber piece"}, {"music_length_ms", 45000}};
    const auto ok = musicGenerateRequestFromJson(json);
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_STREQ(ok.getValue()->requestKind(), "prompt");
    EXPECT_EQ(ok.getValue()->musicLengthMs, 45000);
    EXPECT_EQ(ok.getValue()->modelId, "music_v2_5");
    EXPECT_TRUE(ok.getValue()->scriptId.empty());

    json.erase("music_length_ms");
    EXPECT_NE(messageOf(json).find("music_length_ms is required"), std::string::npos);
    json["music_length_ms"] = 2999;
    EXPECT_NE(messageOf(json).find("music_length_ms"), std::string::npos);
    EXPECT_NE(messageOf({{"prompt", "x"}, {"music_length_ms", 5000}, {"script_id", "nope"}}).find("script_id"),
              std::string::npos); // dialog binding is not part of this contract
}

TEST(MusicContracts, SectionsModeIsTheRefinementBuilderInput) {
    nlohmann::json json = {{"piece_id", PIECE_ID},
                           {"base_version_id", VERSION_ID},
                           {"sections", {section("[Intro]", 8000), section("[Verse] {warmer}", 20000)}},
                           {"keep", {0}},
                           {"condition_strength", "high"},
                           {"seed", 11}};
    const auto ok = musicGenerateRequestFromJson(json);
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    const auto request = ok.getValue().value();
    EXPECT_STREQ(request.requestKind(), "sections");
    ASSERT_TRUE(request.sections.has_value());
    EXPECT_EQ(request.sections->size(), 2u);
    EXPECT_EQ(request.keep, (std::vector<std::size_t>{0}));
    EXPECT_EQ(request.conditionStrength, "high");
    EXPECT_EQ(request.seed.value(), 11);
    EXPECT_EQ(request.pieceId, PIECE_ID);

    auto bad = json;
    bad["prompt"] = "also";
    EXPECT_NE(messageOf(bad).find("prompt cannot be combined with sections"), std::string::npos);
    bad = json;
    bad["music_length_ms"] = 5000;
    EXPECT_NE(messageOf(bad).find("music_length_ms only applies to prompt mode"), std::string::npos);
    bad = json;
    bad["keep"] = {5};
    EXPECT_NE(messageOf(bad).find("keep[5] is beyond the last section"), std::string::npos);
    bad = json;
    bad.erase("base_version_id");
    EXPECT_NE(messageOf(bad).find("keep requires base_version_id"), std::string::npos);
    bad = json;
    bad.erase("piece_id");
    EXPECT_NE(messageOf(bad).find("base_version_id requires piece_id"), std::string::npos);
    bad = json;
    bad["condition_strength"] = "max";
    EXPECT_NE(messageOf(bad).find("condition_strength must be"), std::string::npos);
    bad = json;
    bad["sections"][0]["conditioning_ref"] = {{"song_id", "s"}, {"range", {{"start_ms", 0}, {"end_ms", 5000}}}};
    EXPECT_NE(messageOf(bad).find("sections[0]"), std::string::npos); // sections are editable content only

    // keep / base_version_id / condition_strength are meaningless outside sections mode.
    EXPECT_NE(messageOf({{"prompt", "x"}, {"music_length_ms", 5000}, {"keep", nlohmann::json::array()}})
                  .find("keep only applies to sections mode"),
              std::string::npos);
}

TEST(MusicContracts, PlanModeStillWorksWithoutADialog) {
    nlohmann::json json = {{"composition_plan", {{"chunks", {section("[A]", 5000)}}}}, {"seed", 3}};
    const auto ok = musicGenerateRequestFromJson(json);
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_STREQ(ok.getValue()->requestKind(), "composition_plan");
    json["music_length_ms"] = 5000;
    EXPECT_NE(messageOf(json).find("music_length_ms only applies to prompt mode"), std::string::npos);
}

/// Stored as job details and re-parsed by the worker: every mode must
/// round-trip to an identical serialisation.
TEST(MusicContracts, GenerateRequestRoundTripsExactlyInEveryMode) {
    const std::vector<nlohmann::json> inputs = {
        {{"prompt", "underscore"},
         {"music_length_ms", 12000},
         {"generation_mode", "loop"},
         {"force_instrumental", false},
         {"finetune_id", "ft"},
         {"finetune_strength", 0.5}},
        {{"composition_plan", {{"chunks", {section("[A]", 5000)}}}}, {"seed", 3}, {"model_id", "music_v2"}},
        {{"piece_id", PIECE_ID},
         {"base_version_id", VERSION_ID},
         {"sections", {section("[Intro]", 8000), section("[Verse]", 9000)}},
         {"keep", {1}},
         {"seed", 4}},
        {{"sections", {section("[Solo]", 8000)}}}, // brand-new piece, no base
    };
    for (const auto &input : inputs) {
        const auto first = musicGenerateRequestFromJson(input);
        ASSERT_TRUE(first.isSuccess()) << first.getError()->getMessage() << " for " << input.dump();
        const auto serialized = musicGenerateRequestToJson(first.getValue().value());
        const auto again = musicGenerateRequestFromJson(serialized);
        ASSERT_TRUE(again.isSuccess()) << again.getError()->getMessage() << " for " << serialized.dump();
        EXPECT_EQ(musicGenerateRequestToJson(again.getValue().value()), serialized);
    }
}

TEST(MusicContracts, SaveRequestNeedsATitleForANewPieceOnly) {
    auto ok = musicSaveRequestFromJson({{"title", "Algorithm Divine"}, {"notes", ""}});
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_FALSE(ok.getValue()->sections.has_value());
    ok = musicSaveRequestFromJson({{"piece_id", PIECE_ID}, {"sections", {section("[A]", 5000)}}});
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_EQ(ok.getValue()->sections->size(), 1u);
    EXPECT_FALSE(musicSaveRequestFromJson({{"notes", "no title"}}).isSuccess());
    EXPECT_FALSE(musicSaveRequestFromJson({{"piece_id", "nope"}}).isSuccess());
    EXPECT_FALSE(musicSaveRequestFromJson({{"title", "x"}, {"mystery", 1}}).isSuccess());
}

TEST(MusicContracts, UpdateRequestMustChangeSomething) {
    EXPECT_FALSE(musicPieceUpdateRequestFromJson(nlohmann::json::object()).isSuccess());
    auto ok = musicPieceUpdateRequestFromJson({{"current_version_id", VERSION_ID}});
    ASSERT_TRUE(ok.isSuccess()) << ok.getError()->getMessage();
    EXPECT_FALSE(ok.getValue()->title.has_value());
    EXPECT_EQ(ok.getValue()->currentVersionId.value(), VERSION_ID);
    EXPECT_FALSE(musicPieceUpdateRequestFromJson({{"current_version_id", "nope"}}).isSuccess());
    EXPECT_FALSE(musicPieceUpdateRequestFromJson({{"title", ""}}).isSuccess());
}

TEST(MusicContracts, RecipeCarriesLibraryFieldsOnlyWhenSet) {
    DialogMusicRecipe recipe;
    recipe.modelId = "music_v2_5";
    recipe.requestKind = "sections";
    auto json = dialogMusicRecipeToJson(recipe);
    EXPECT_FALSE(json.contains("sections"));
    EXPECT_FALSE(json.contains("piece_id"));
    recipe.sections = nlohmann::json::array({section("[A]", 5000)});
    recipe.pieceId = PIECE_ID;
    recipe.baseVersionId = VERSION_ID;
    json = dialogMusicRecipeToJson(recipe);
    EXPECT_EQ(json["sections"].size(), 1u);
    EXPECT_EQ(json["piece_id"], PIECE_ID);
    EXPECT_EQ(json["base_version_id"], VERSION_ID);
}

} // namespace creatures::api
