#include <base64.hpp>
#include <gtest/gtest.h>

#include <limits>

#include "model/LogItem.h"
#include "model/StreamFrame.h"
#include "model/VirtualStatusLights.h"

namespace creatures {

TEST(LogItemJson, PreservesWireShapeAndRejectsUnknownLevel) {
    const LogItem item{"2026-08-22T12:00:00Z", LogLevel::warn, "Careful", "server", 42};
    EXPECT_EQ(logItemToJson(item), (nlohmann::json{{"timestamp", item.timestamp},
                                                   {"level", "warning"},
                                                   {"message", item.message},
                                                   {"logger_name", item.logger_name},
                                                   {"thread_id", item.thread_id}}));
    const auto parsed = logItemFromJson(logItemToJson(item));
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->level, LogLevel::warn);
    auto malformed = logItemToJson(item);
    malformed["level"] = "unexpected";
    EXPECT_FALSE(logItemFromJson(malformed).isSuccess());
}

TEST(VirtualStatusLightsJson, RoundTripsExactBooleanShape) {
    const VirtualStatusLights lights{true, false, true, false};
    const auto parsed = virtualStatusLightsFromJson(virtualStatusLightsToJson(lights));
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->running, lights.running);
    EXPECT_EQ(parsed.getValue()->dmx, lights.dmx);
    EXPECT_EQ(parsed.getValue()->streaming, lights.streaming);
    EXPECT_EQ(parsed.getValue()->animation_playing, lights.animation_playing);
}

TEST(StreamFrameJson, EnforcesSmallValidDmxFrames) {
    const StreamFrame frame{"11111111-1111-4111-8111-111111111111", 1, base64::to_base64("\x01\x02")};
    const auto parsed = streamFrameFromJson(streamFrameToJson(frame));
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->data, frame.data);

    auto malformed = streamFrameToJson(frame);
    malformed["universe"] = 0;
    EXPECT_FALSE(streamFrameFromJson(malformed).isSuccess());
    malformed = streamFrameToJson(frame);
    malformed["universe"] = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) + 1;
    EXPECT_FALSE(streamFrameFromJson(malformed).isSuccess());
    malformed = streamFrameToJson(frame);
    malformed["universe"] = 1.5;
    EXPECT_FALSE(streamFrameFromJson(malformed).isSuccess());
    malformed = streamFrameToJson(frame);
    malformed["data"] = "not base64!";
    EXPECT_FALSE(streamFrameFromJson(malformed).isSuccess());
    malformed = streamFrameToJson(frame);
    malformed["data"] = base64::to_base64(std::string(MAX_STREAM_FRAME_DECODED_BYTES + 1, 'x'));
    EXPECT_FALSE(streamFrameFromJson(malformed).isSuccess());
}

} // namespace creatures
