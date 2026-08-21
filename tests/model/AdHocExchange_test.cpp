#include <gtest/gtest.h>

#include "model/AdHocExchange.h"

namespace {

using creatures::AdHocExchange;
using creatures::AdHocExchangePart;

AdHocExchange makeExchange() {
    AdHocExchange exchange;
    exchange.session_id = "11111111-2222-3333-4444-555555555555";
    exchange.creature_id = "creature-1";
    exchange.creature_name = "Beaky";
    exchange.status = creatures::EXCHANGE_STATUS_READY;
    exchange.title = "Beaky - 20260820143012 - somebody-at-the-door";
    exchange.transcript = "Somebody's at the door! I can see them!";
    exchange.sound_file = "/tmp/creature-adhoc/1111/1111.wav";
    exchange.duration_ms = 5250;
    exchange.finished_at_ms = 1787000000000;
    exchange.parts = {{1, "anim-1", "Somebody's at the door!", 2500}, {2, "anim-2", "I can see them!", 2750}};
    return exchange;
}

TEST(AdHocExchange, JsonRoundTripPreservesEverything) {
    const auto original = makeExchange();
    auto parsed = creatures::adHocExchangeFromJson(creatures::adHocExchangeToJson(original));
    ASSERT_TRUE(parsed.isSuccess());
    EXPECT_EQ(original, parsed.getValue().value());
}

TEST(AdHocExchange, MissingOptionalFieldsDefaultSanely) {
    // The bare shape written at /start: just ids and a status.
    nlohmann::json json = {{"session_id", "abc"}, {"creature_id", "c-1"}};
    auto parsed = creatures::adHocExchangeFromJson(json);
    ASSERT_TRUE(parsed.isSuccess());
    const auto exchange = parsed.getValue().value();
    EXPECT_EQ(std::string{creatures::EXCHANGE_STATUS_STREAMING}, exchange.status);
    EXPECT_TRUE(exchange.parts.empty());
    EXPECT_EQ(0u, exchange.duration_ms);
    EXPECT_EQ(0, exchange.finished_at_ms);
}

TEST(AdHocExchange, RejectsJsonWithoutRequiredIds) {
    EXPECT_FALSE(creatures::adHocExchangeFromJson(nlohmann::json{{"creature_id", "c-1"}}).isSuccess());
    EXPECT_FALSE(creatures::adHocExchangeFromJson(nlohmann::json{{"session_id", "abc"}}).isSuccess());
}

} // namespace
