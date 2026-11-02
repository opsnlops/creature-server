#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "server/voice/StreamingTTSClient.h"

namespace creatures::voice {

// A LIVE gate for the text-to-speech socket, added when its hand-rolled TLS and
// framing were replaced with IXWebSocket (issue #251). The equivalent gate for
// the dialogue socket found a process-killing bug on its first run, so this path
// — which has exactly one caller but drives it through the job-based ad-hoc
// route — gets the same treatment rather than being trusted to compile.
//
// Skipped unless explicitly opted into, so CI never reaches the network:
//
//   CREATURE_LIVE_ELEVENLABS=1 VOICE_API_KEY=... \
//       ./creature-server-test --gtest_filter='StreamingTTSClientLive.*'

namespace {

/// Beaky's actual voice (Gigi). Override with CREATURE_LIVE_VOICE_ID.
constexpr const char *kDefaultVoiceId = "n7Wi4g1bhpw4Bs8HK5ph";

std::string liveKeyOrEmpty() {
    const char *gate = std::getenv("CREATURE_LIVE_ELEVENLABS");
    const char *key = std::getenv("VOICE_API_KEY");
    if (gate == nullptr || std::string(gate) != "1") {
        return {};
    }
    if (key == nullptr || *key == '\0') {
        return {};
    }
    return std::string(key);
}

std::string liveVoiceId() {
    const char *voiceId = std::getenv("CREATURE_LIVE_VOICE_ID");
    return (voiceId != nullptr && *voiceId != '\0') ? std::string(voiceId) : std::string(kDefaultVoiceId);
}

} // namespace

TEST(StreamingTTSClientLive, StreamsAudioAndAlignment) {
    const auto apiKey = liveKeyOrEmpty();
    if (apiKey.empty()) {
        GTEST_SKIP() << "set CREATURE_LIVE_ELEVENLABS=1 and VOICE_API_KEY to run the live text-to-speech gate";
    }

    StreamingTTSClient client;
    auto rendered = client.generateSpeech(apiKey, liveVoiceId(), "eleven_flash_v2_5",
                                          "Testing one two three, this is a check of the streaming socket.",
                                          "pcm_48000", 0.5f, 0.72f);
    ASSERT_TRUE(rendered.isSuccess()) << rendered.getError()->getMessage();
    const auto result = rendered.getValue().value();

    // pcm_48000 is 16-bit mono at 48 kHz, so ~96 kB per second of audio.
    EXPECT_GT(result.audioData.size(), 96000u) << "less than a second of audio";
    EXPECT_GT(result.audioDurationSeconds, 1.0);
    EXPECT_EQ(result.audioFormat, "pcm_48000");

    // Alignment needs sync_alignment accepted AND the camelCase dialect to
    // match. Either mistake gives good audio and a motionless beak.
    EXPECT_GT(result.charTimings.size(), 20u) << "no alignment — lip sync would be dead";
    EXPECT_FALSE(result.alignmentText.empty());
}

// The same client must be reusable for a second sentence. Each call owns its own
// connection, and the accumulating state has to be reset between them — a leak
// there would show up as the second result carrying the first one's audio.
TEST(StreamingTTSClientLive, IsReusableAndDoesNotCarryStateBetweenCalls) {
    const auto apiKey = liveKeyOrEmpty();
    if (apiKey.empty()) {
        GTEST_SKIP() << "set CREATURE_LIVE_ELEVENLABS=1 and VOICE_API_KEY to run the live text-to-speech gate";
    }
    const auto voiceId = liveVoiceId();

    StreamingTTSClient client;
    auto first = client.generateSpeech(apiKey, voiceId, "eleven_flash_v2_5",
                                       "This is a much longer first sentence, with a good deal more to say in it.",
                                       "pcm_48000", 0.5f, 0.72f);
    ASSERT_TRUE(first.isSuccess()) << first.getError()->getMessage();
    const auto firstSize = first.getValue().value().audioData.size();

    auto second = client.generateSpeech(apiKey, voiceId, "eleven_flash_v2_5", "Short.", "pcm_48000", 0.5f, 0.72f);
    ASSERT_TRUE(second.isSuccess()) << second.getError()->getMessage();
    const auto secondSize = second.getValue().value().audioData.size();

    EXPECT_GT(secondSize, 0u);
    EXPECT_LT(secondSize, firstSize) << "the short sentence came back at least as long as the long one, so state "
                                        "leaked between calls";
}

// The v3/v4 families are refused by this socket with HTTP 400, which has to
// surface as a clean error rather than a hang or a crash.
TEST(StreamingTTSClientLive, ReportsARefusedModelRatherThanHanging) {
    const auto apiKey = liveKeyOrEmpty();
    if (apiKey.empty()) {
        GTEST_SKIP() << "set CREATURE_LIVE_ELEVENLABS=1 and VOICE_API_KEY to run the live text-to-speech gate";
    }

    StreamingTTSClient client;
    auto rendered = client.generateSpeech(apiKey, liveVoiceId(), "eleven_v4_turbo", "This model is not served here.",
                                          "pcm_48000", 0.5f, 0.72f);
    EXPECT_FALSE(rendered.isSuccess()) << "the text-to-speech socket should refuse eleven_v4_turbo";
}

} // namespace creatures::voice
