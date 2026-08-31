#include <gtest/gtest.h>

#include "api/StreamingAdHocContracts.h"

namespace creatures::api {
namespace {

constexpr const char *CREATURE_ID = "ABCDEF12-3456-4ABC-8DEF-1234567890AB";
constexpr const char *SESSION_ID = "00000000-0000-4000-8000-000000000001";
constexpr const char *UPPERCASE_SESSION_ID = "ABCDEF12-3456-4ABC-8DEF-1234567890AB";

TEST(StreamingAdHocContracts, ParsesStrictStartRequest) {
    const auto defaults = streamingAdHocStartRequestFromJson({{"creature_id", CREATURE_ID}});
    ASSERT_TRUE(defaults.isSuccess()) << defaults.getError()->getMessage();
    EXPECT_EQ(defaults.getValue()->creatureId, CREATURE_ID);
    EXPECT_TRUE(defaults.getValue()->resumePlaylist);

    const auto explicitValue =
        streamingAdHocStartRequestFromJson({{"creature_id", CREATURE_ID}, {"resume_playlist", false}});
    ASSERT_TRUE(explicitValue.isSuccess()) << explicitValue.getError()->getMessage();
    EXPECT_FALSE(explicitValue.getValue()->resumePlaylist);

    EXPECT_FALSE(streamingAdHocStartRequestFromJson({{"creature_id", "not-a-uuid"}}).isSuccess());
    EXPECT_FALSE(streamingAdHocStartRequestFromJson({{"creature_id", CREATURE_ID}, {"extra", true}}).isSuccess());
    EXPECT_FALSE(
        streamingAdHocStartRequestFromJson({{"creature_id", CREATURE_ID}, {"resume_playlist", 1}}).isSuccess());
}

TEST(StreamingAdHocContracts, ParsesAndBoundsTextRequest) {
    const auto parsed = streamingAdHocTextRequestFromJson({{"session_id", SESSION_ID}, {"text", "Hello there"}});
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->sessionId, SESSION_ID);
    EXPECT_EQ(parsed.getValue()->text, "Hello there");

    EXPECT_FALSE(streamingAdHocTextRequestFromJson({{"session_id", "bad"}, {"text", "Hello"}}).isSuccess());
    EXPECT_FALSE(streamingAdHocTextRequestFromJson({{"session_id", SESSION_ID}, {"text", ""}}).isSuccess());
    EXPECT_FALSE(streamingAdHocTextRequestFromJson(
                     {{"session_id", SESSION_ID}, {"text", std::string(MAX_STREAMING_AD_HOC_TEXT_BYTES + 1, 'x')}})
                     .isSuccess());

    const auto uppercase = streamingAdHocTextRequestFromJson({{"session_id", UPPERCASE_SESSION_ID}, {"text", "Hello"}});
    ASSERT_TRUE(uppercase.isSuccess()) << uppercase.getError()->getMessage();
    EXPECT_EQ(uppercase.getValue()->sessionId, "abcdef12-3456-4abc-8def-1234567890ab");
}

TEST(StreamingAdHocContracts, ParsesStrictFinishRequest) {
    const auto parsed = streamingAdHocFinishRequestFromJson({{"session_id", UPPERCASE_SESSION_ID}});
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->sessionId, "abcdef12-3456-4abc-8def-1234567890ab");
    EXPECT_FALSE(streamingAdHocFinishRequestFromJson({{"session_id", SESSION_ID}, {"extra", true}}).isSuccess());
}

TEST(StreamingAdHocContracts, ParsesExchangeLimitWithoutPartialNumbers) {
    EXPECT_EQ(adHocExchangeLimitFromString("1").getValue().value(), 1);
    EXPECT_EQ(adHocExchangeLimitFromString("500").getValue().value(), 500);
    EXPECT_FALSE(adHocExchangeLimitFromString("0").isSuccess());
    EXPECT_FALSE(adHocExchangeLimitFromString("501").isSuccess());
    EXPECT_FALSE(adHocExchangeLimitFromString("1x").isSuccess());
    EXPECT_FALSE(adHocExchangeLimitFromString(" 1").isSuccess());
    EXPECT_FALSE(adHocExchangeLimitFromString("").isSuccess());
}

TEST(StreamingAdHocContracts, SerializesStableResponseShapes) {
    EXPECT_EQ(streamingAdHocStartResponseToJson({SESSION_ID, "started", "Ready"}),
              (nlohmann::json{{"session_id", SESSION_ID}, {"status", "started"}, {"message", "Ready"}}));
    EXPECT_EQ(streamingAdHocTextResponseToJson({SESSION_ID, "ok", 3}),
              (nlohmann::json{{"session_id", SESSION_ID}, {"status", "ok"}, {"chunks_received", 3}}));
    EXPECT_EQ(streamingAdHocFinishResponseToJson({SESSION_ID, "completed", "Done", CREATURE_ID, true, "ready", 2, 2}),
              (nlohmann::json{{"session_id", SESSION_ID},
                              {"status", "completed"},
                              {"message", "Done"},
                              {"animation_id", CREATURE_ID},
                              {"playback_triggered", true},
                              {"exchange_status", "ready"},
                              {"parts_rendered", 2},
                              {"parts_total", 2}}));
}

TEST(StreamingAdHocContracts, ExchangeResponseOmitsStoragePathAndAbsentFinishTime) {
    AdHocExchange exchange;
    exchange.session_id = "ABCDEF12-3456-4ABC-8DEF-1234567890AB";
    exchange.creature_id = CREATURE_ID;
    exchange.creature_name = "Beaky";
    exchange.sound_file = "/private/server/path.wav";
    exchange.parts = {{1, "ABCDEF12-3456-4ABC-8DEF-1234567890AC", "Hello", 250}};

    const auto json = adHocExchangeResponseToJson(exchange, "2026-08-30T12:00:00Z", std::nullopt);
    EXPECT_EQ(json["session_id"], "abcdef12-3456-4abc-8def-1234567890ab");
    EXPECT_EQ(json["creature_id"], "abcdef12-3456-4abc-8def-1234567890ab");
    EXPECT_EQ(json["parts"][0]["animation_id"], "abcdef12-3456-4abc-8def-1234567890ac");
    EXPECT_FALSE(json.contains("sound_file"));
    EXPECT_FALSE(json.contains("finished_at"));
}

} // namespace
} // namespace creatures::api
