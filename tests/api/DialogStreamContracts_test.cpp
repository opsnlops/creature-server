#include <gtest/gtest.h>

#include <string>

#include "api/DialogStreamContracts.h"

namespace creatures::api {
namespace {

constexpr const char *BEAKY = "ABCDEF12-3456-4ABC-8DEF-1234567890AB";
constexpr const char *MANGO = "abcdef12-3456-4abc-8def-1234567890ac";
constexpr const char *STAGE_ID = "00000000-0000-4000-8000-00000000000a";
constexpr const char *SESSION_ID = "00000000-0000-4000-8000-000000000001";

nlohmann::json validStart() {
    return {{"creature_ids", nlohmann::json::array({BEAKY, MANGO})}, {"stage_id", STAGE_ID}};
}

TEST(DialogStreamContracts, ParsesStartRequestKeepingSubmittedSpelling) {
    const auto parsed = dialogStreamStartRequestFromJson(validStart());
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    const auto request = parsed.getValue().value();
    ASSERT_EQ(request.creatureIds.size(), 2u);
    EXPECT_EQ(request.creatureIds[0], BEAKY); // uppercase preserved for the Mongo lookup
    EXPECT_EQ(request.creatureIds[1], MANGO);
    EXPECT_EQ(request.stageId, STAGE_ID);
    EXPECT_TRUE(request.resumePlaylist);

    auto explicitValue = validStart();
    explicitValue["resume_playlist"] = false;
    const auto parsedExplicit = dialogStreamStartRequestFromJson(explicitValue);
    ASSERT_TRUE(parsedExplicit.isSuccess());
    EXPECT_FALSE(parsedExplicit.getValue()->resumePlaylist);
}

TEST(DialogStreamContracts, ASingleCreatureDialogIsAllowed) {
    nlohmann::json json = {{"creature_ids", nlohmann::json::array({BEAKY})}, {"stage_id", STAGE_ID}};
    const auto parsed = dialogStreamStartRequestFromJson(json);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->creatureIds.size(), 1u);
}

TEST(DialogStreamContracts, StartRequestInsistsOnAStage) {
    auto noStage = validStart();
    noStage.erase("stage_id");
    EXPECT_FALSE(dialogStreamStartRequestFromJson(noStage).isSuccess());

    auto badStage = validStart();
    badStage["stage_id"] = "mainstage";
    EXPECT_FALSE(dialogStreamStartRequestFromJson(badStage).isSuccess());
}

TEST(DialogStreamContracts, StartRequestBoundsAndValidatesTheCast) {
    auto empty = validStart();
    empty["creature_ids"] = nlohmann::json::array();
    EXPECT_FALSE(dialogStreamStartRequestFromJson(empty).isSuccess());

    auto missing = validStart();
    missing.erase("creature_ids");
    EXPECT_FALSE(dialogStreamStartRequestFromJson(missing).isSuccess());

    auto notAnArray = validStart();
    notAnArray["creature_ids"] = BEAKY;
    EXPECT_FALSE(dialogStreamStartRequestFromJson(notAnArray).isSuccess());

    auto notAUuid = validStart();
    notAUuid["creature_ids"] = nlohmann::json::array({BEAKY, "mango"});
    EXPECT_FALSE(dialogStreamStartRequestFromJson(notAUuid).isSuccess());

    auto notAString = validStart();
    notAString["creature_ids"] = nlohmann::json::array({BEAKY, 7});
    EXPECT_FALSE(dialogStreamStartRequestFromJson(notAString).isSuccess());

    // Same creature twice — even spelled differently — is a mistake.
    auto duplicate = validStart();
    duplicate["creature_ids"] = nlohmann::json::array({BEAKY, "abcdef12-3456-4abc-8def-1234567890ab"});
    EXPECT_FALSE(dialogStreamStartRequestFromJson(duplicate).isSuccess());

    auto tooMany = validStart();
    tooMany["creature_ids"] = nlohmann::json::array();
    for (std::size_t i = 0; i <= MAX_DIALOG_STREAM_PARTICIPANTS; ++i) {
        tooMany["creature_ids"].push_back("00000000-0000-4000-8000-0000000000" + std::to_string(10 + i));
    }
    EXPECT_FALSE(dialogStreamStartRequestFromJson(tooMany).isSuccess());

    auto unknownField = validStart();
    unknownField["autoplay"] = true;
    EXPECT_FALSE(dialogStreamStartRequestFromJson(unknownField).isSuccess());
}

TEST(DialogStreamContracts, ParsesAndBoundsTurnRequest) {
    const auto parsed = dialogStreamTurnRequestFromJson(
        {{"session_id", "00000000-0000-4000-8000-00000000000B"}, {"creature_id", BEAKY}, {"text", "Hello there"}});
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->sessionId, "00000000-0000-4000-8000-00000000000b"); // session ids canonicalize
    EXPECT_EQ(parsed.getValue()->creatureId, BEAKY);                                 // creature ids do not
    EXPECT_EQ(parsed.getValue()->text, "Hello there");

    EXPECT_FALSE(dialogStreamTurnRequestFromJson({{"session_id", SESSION_ID}, {"text", "Hi"}}).isSuccess());
    EXPECT_FALSE(dialogStreamTurnRequestFromJson({{"session_id", SESSION_ID}, {"creature_id", "beaky"}, {"text", "Hi"}})
                     .isSuccess());
    EXPECT_FALSE(dialogStreamTurnRequestFromJson({{"session_id", SESSION_ID}, {"creature_id", BEAKY}, {"text", ""}})
                     .isSuccess());
    EXPECT_FALSE(dialogStreamTurnRequestFromJson({{"session_id", SESSION_ID},
                                                  {"creature_id", BEAKY},
                                                  {"text", std::string(MAX_DIALOG_STREAM_TEXT_BYTES + 1, 'x')}})
                     .isSuccess());
    EXPECT_FALSE(dialogStreamTurnRequestFromJson(
                     {{"session_id", SESSION_ID}, {"creature_id", BEAKY}, {"text", "Hi"}, {"extra", 1}})
                     .isSuccess());
}

TEST(DialogStreamContracts, ParsesFinishRequest) {
    const auto parsed = dialogStreamFinishRequestFromJson({{"session_id", SESSION_ID}});
    ASSERT_TRUE(parsed.isSuccess());
    EXPECT_EQ(parsed.getValue()->sessionId, SESSION_ID);
    EXPECT_FALSE(dialogStreamFinishRequestFromJson({{"session_id", "nope"}}).isSuccess());
    EXPECT_FALSE(dialogStreamFinishRequestFromJson({{"session_id", SESSION_ID}, {"extra", 1}}).isSuccess());
}

TEST(DialogStreamContracts, ResponseShapesAreStable) {
    EXPECT_EQ(dialogStreamStartResponseToJson({SESSION_ID, "started", "go", {BEAKY, MANGO}, STAGE_ID}),
              (nlohmann::json{{"session_id", SESSION_ID},
                              {"status", "started"},
                              {"message", "go"},
                              {"creature_ids", nlohmann::json::array({"abcdef12-3456-4abc-8def-1234567890ab", MANGO})},
                              {"stage_id", STAGE_ID}}));
    EXPECT_EQ(dialogStreamTurnResponseToJson({SESSION_ID, "ok", 3}),
              (nlohmann::json{{"session_id", SESSION_ID}, {"status", "ok"}, {"turns_received", 3}}));
    EXPECT_EQ(dialogStreamFinishResponseToJson({SESSION_ID, "completed", "done", MANGO, BEAKY, true, "ready", 2, 2}),
              (nlohmann::json{{"session_id", SESSION_ID},
                              {"status", "completed"},
                              {"message", "done"},
                              {"animation_id", MANGO},
                              {"last_turn_animation_id", "abcdef12-3456-4abc-8def-1234567890ab"},
                              {"playback_triggered", true},
                              {"exchange_status", "ready"},
                              {"parts_rendered", 2},
                              {"parts_total", 2}}));
}

} // namespace
} // namespace creatures::api
