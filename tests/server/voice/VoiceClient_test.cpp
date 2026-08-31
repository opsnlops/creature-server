#include <gtest/gtest.h>

#include "server/voice/VoiceClient.h"

namespace creatures::voice {

TEST(VoiceClientResponse, ParsesAndSortsVoicesFromOwnedDocument) {
    const auto result = parseVoiceListResponse(R"({"voices":[
        {"voice_id":"voice-z","name":"Zebra"},
        {"voice_id":"voice-a","name":"Alpaca"}
    ]})");

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    ASSERT_EQ(result.getValue()->size(), 2U);
    EXPECT_EQ(result.getValue()->at(0).name, "Alpaca");
    EXPECT_EQ(result.getValue()->at(0).voiceId, "voice-a");
    EXPECT_EQ(result.getValue()->at(1).name, "Zebra");
}

TEST(VoiceClientResponse, RejectsMalformedAndUnexpectedShapes) {
    EXPECT_FALSE(parseVoiceListResponse("not json").isSuccess());
    EXPECT_FALSE(parseVoiceListResponse(R"({"voices":{}})").isSuccess());
    EXPECT_FALSE(parseVoiceListResponse(R"({"voices":[{"voice_id":"voice-a"}]})").isSuccess());
}

} // namespace creatures::voice
