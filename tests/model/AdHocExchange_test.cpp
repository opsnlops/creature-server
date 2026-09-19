#include <gtest/gtest.h>

#include "model/AdHocExchange.h"

namespace {

using creatures::AdHocExchange;
using creatures::AdHocExchangePart;

AdHocExchange makeExchange() {
    AdHocExchange exchange;
    exchange.session_id = "11111111-2222-3333-4444-555555555555";
    exchange.creature_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    exchange.creature_name = "Beaky";
    exchange.status = creatures::EXCHANGE_STATUS_READY;
    exchange.title = "Beaky - 20260820143012 - somebody-at-the-door";
    exchange.transcript = "Somebody's at the door! I can see them!";
    exchange.sound_file = "/tmp/creature-adhoc/1111/1111.wav";
    exchange.duration_ms = 5250;
    exchange.finished_at_ms = 1787000000000;
    exchange.parts = {{1, "10000000-0000-4000-8000-000000000001", "Somebody's at the door!", 2500},
                      {2, "10000000-0000-4000-8000-000000000002", "I can see them!", 2750}};
    return exchange;
}

AdHocExchange makeDialogExchange() {
    auto exchange = makeExchange();
    exchange.stage_id = "20000000-0000-4000-8000-000000000001";
    exchange.participants = {{"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee", "Beaky", 3},
                             {"bbbbbbbb-bbbb-4ccc-8ddd-eeeeeeeeeeee", "Mango", 5}};
    exchange.parts[0].creature_id = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    exchange.parts[0].creature_name = "Beaky";
    exchange.parts[1].creature_id = "bbbbbbbb-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    exchange.parts[1].creature_name = "Mango";
    return exchange;
}

TEST(AdHocExchange, JsonRoundTripPreservesEverything) {
    const auto original = makeExchange();
    auto parsed = creatures::adHocExchangeFromJson(creatures::adHocExchangeToJson(original));
    ASSERT_TRUE(parsed.isSuccess());
    EXPECT_EQ(original, parsed.getValue().value());
}

TEST(AdHocExchange, SingleCreatureRecordsKeepTheirPreDialogShape) {
    // Readers of the old shape (and the byte-for-byte record format) must not
    // see the #186 fields unless a dialog actually wrote them.
    const auto json = creatures::adHocExchangeToJson(makeExchange());
    EXPECT_FALSE(json.contains("participants"));
    EXPECT_FALSE(json.contains("stage_id"));
    EXPECT_FALSE(json["parts"][0].contains("creature_id"));
}

TEST(AdHocExchange, DialogRoundTripPreservesCastStageAndSpeakers) {
    const auto original = makeDialogExchange();
    const auto json = creatures::adHocExchangeToJson(original);
    ASSERT_EQ(json["participants"].size(), 2u);
    EXPECT_EQ(json["stage_id"], "20000000-0000-4000-8000-000000000001");
    EXPECT_EQ(json["parts"][1]["creature_name"], "Mango");
    auto parsed = creatures::adHocExchangeFromJson(json);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(original, parsed.getValue().value());
}

TEST(AdHocExchange, RejectsMalformedDialogFields) {
    const auto valid = creatures::adHocExchangeToJson(makeDialogExchange());

    auto badStage = valid;
    badStage["stage_id"] = "mainstage";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(badStage).isSuccess());

    auto badParticipantId = valid;
    badParticipantId["participants"][0]["creature_id"] = "beaky";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(badParticipantId).isSuccess());

    auto unknownParticipantField = valid;
    unknownParticipantField["participants"][0]["voice"] = "x";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(unknownParticipantField).isSuccess());

    auto badPartSpeaker = valid;
    badPartSpeaker["parts"][0]["creature_id"] = "beaky";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(badPartSpeaker).isSuccess());

    auto tooManyParticipants = valid;
    tooManyParticipants["participants"] = nlohmann::json::array();
    for (std::size_t i = 0; i <= creatures::MAX_AD_HOC_EXCHANGE_PARTICIPANTS; ++i) {
        tooManyParticipants["participants"].push_back(
            {{"creature_id", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"}, {"creature_name", "x"}, {"audio_channel", 1}});
    }
    EXPECT_FALSE(creatures::adHocExchangeFromJson(tooManyParticipants).isSuccess());
}

TEST(AdHocExchange, MissingOptionalFieldsDefaultSanely) {
    // The bare shape written at /start: just ids and a status.
    nlohmann::json json = {{"session_id", "11111111-2222-4333-8444-555555555555"},
                           {"creature_id", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"}};
    auto parsed = creatures::adHocExchangeFromJson(json);
    ASSERT_TRUE(parsed.isSuccess());
    const auto exchange = parsed.getValue().value();
    EXPECT_EQ(std::string{creatures::EXCHANGE_STATUS_STREAMING}, exchange.status);
    EXPECT_TRUE(exchange.parts.empty());
    EXPECT_EQ(0u, exchange.duration_ms);
    EXPECT_EQ(0, exchange.finished_at_ms);
}

TEST(AdHocExchange, RejectsJsonWithoutRequiredIds) {
    EXPECT_FALSE(
        creatures::adHocExchangeFromJson(nlohmann::json{{"creature_id", "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee"}})
            .isSuccess());
    EXPECT_FALSE(
        creatures::adHocExchangeFromJson(nlohmann::json{{"session_id", "11111111-2222-4333-8444-555555555555"}})
            .isSuccess());
}

TEST(AdHocExchange, RejectsMalformedPersistedFields) {
    const auto valid = creatures::adHocExchangeToJson(makeExchange());

    auto unknownField = valid;
    unknownField["unexpected"] = true;
    EXPECT_FALSE(creatures::adHocExchangeFromJson(unknownField).isSuccess());

    auto unknownStatus = valid;
    unknownStatus["status"] = "surprising";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(unknownStatus).isSuccess());

    auto wrongDurationType = valid;
    wrongDurationType["duration_ms"] = "5250";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(wrongDurationType).isSuccess());

    auto invalidAnimationId = valid;
    invalidAnimationId["parts"][0]["animation_id"] = "anim-1";
    EXPECT_FALSE(creatures::adHocExchangeFromJson(invalidAnimationId).isSuccess());
}

TEST(AdHocExchange, BoundsStoredArraysAndText) {
    auto tooManyParts = creatures::adHocExchangeToJson(makeExchange());
    tooManyParts["parts"] = nlohmann::json::array();
    for (std::size_t i = 0; i <= creatures::MAX_AD_HOC_EXCHANGE_PARTS; ++i) {
        tooManyParts["parts"].push_back({{"index", i + 1},
                                         {"animation_id", "10000000-0000-4000-8000-000000000001"},
                                         {"text", "hello"},
                                         {"duration_ms", 1}});
    }
    EXPECT_FALSE(creatures::adHocExchangeFromJson(tooManyParts).isSuccess());

    auto oversizedTranscript = creatures::adHocExchangeToJson(makeExchange());
    oversizedTranscript["transcript"] = std::string(creatures::MAX_AD_HOC_EXCHANGE_TRANSCRIPT_BYTES + 1, 'x');
    EXPECT_FALSE(creatures::adHocExchangeFromJson(oversizedTranscript).isSuccess());
}

} // namespace
