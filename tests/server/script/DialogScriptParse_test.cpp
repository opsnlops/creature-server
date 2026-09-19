#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "model/DialogScript.h"
#include "server/database.h"

using creatures::Database;
using creatures::ServerError;

namespace {

nlohmann::json scriptWithMusic(nlohmann::json prompt) {
    return {{"id", "00000000-0000-4000-8000-000000000002"},
            {"title", "Test - Sound off"},
            {"turns", {{{"creature_id", "00000000-0000-4000-8000-000000000001"}, {"text", "I'm Beaky!"}}}},
            {"background_music",
             {{"sound_file", "dialog/music/test-sound-off--bgm--music--1989acb4-331.wav"},
              {"generation_id", "1989acb4-3314-455d-a10d-7314c8d5e024"},
              {"prompt", std::move(prompt)},
              {"accepted_at", 1789783000000}}}};
}

} // namespace

/// #200 regression: a promoted composition-plan take has no prompt. Requiring
/// one made the script publish fail *after* the WAV was copied, so plan-mode
/// takes could never be accepted (found on prod, 3.47.1).
TEST(DialogScriptParse, AcceptsEmptyBackgroundMusicPromptForPlanModeTakes) {
    auto result = Database::dialogScriptFromJson(scriptWithMusic(""), nullptr);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    ASSERT_TRUE(result.getValue()->background_music.has_value());
    EXPECT_TRUE(result.getValue()->background_music->prompt.empty());
    EXPECT_EQ(result.getValue()->background_music->generation_id, "1989acb4-3314-455d-a10d-7314c8d5e024");
}

TEST(DialogScriptParse, KeepsBackgroundMusicPromptForPromptModeTakes) {
    auto result = Database::dialogScriptFromJson(scriptWithMusic("quiet strings"), nullptr);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_EQ(result.getValue()->background_music->prompt, "quiet strings");
}

TEST(DialogScriptParse, RejectsMissingOrNonStringBackgroundMusicPrompt) {
    auto json = scriptWithMusic("x");
    json["background_music"].erase("prompt");
    auto missing = Database::dialogScriptFromJson(json, nullptr);
    ASSERT_FALSE(missing.isSuccess());
    EXPECT_EQ(missing.getError()->getCode(), ServerError::InvalidData);

    auto wrongType = Database::dialogScriptFromJson(scriptWithMusic(42), nullptr);
    ASSERT_FALSE(wrongType.isSuccess());
    EXPECT_NE(wrongType.getError()->getMessage().find("background_music.prompt"), std::string::npos);
}
