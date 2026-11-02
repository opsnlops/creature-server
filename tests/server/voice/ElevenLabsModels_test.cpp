#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "server/config/Configuration.h"
#include "server/voice/ElevenLabsModels.h"
#include "server/voice/TextToViseme.h"

namespace creatures {
// Defined in TestGlobals.cpp, and null unless a test stands one up — the same
// way DialogCache_test reaches it.
extern std::shared_ptr<Configuration> config;
} // namespace creatures

namespace creatures::voice {

// Issue #220: v3/v4 perform audio tags; the fast models read them aloud.
TEST(ElevenLabsModels, OnlyTheV3AndV4FamiliesPerformAudioTags) {
    EXPECT_TRUE(supportsAudioTags("eleven_v4"));
    EXPECT_TRUE(supportsAudioTags("eleven_v4_turbo"));
    EXPECT_TRUE(supportsAudioTags("eleven_v3"));
    EXPECT_TRUE(supportsAudioTags("eleven_v3_conversational"));
    EXPECT_FALSE(supportsAudioTags("eleven_flash_v2_5"));
    EXPECT_FALSE(supportsAudioTags("eleven_turbo_v2_5"));
    EXPECT_FALSE(supportsAudioTags("eleven_multilingual_v2"));
}

// The ad-hoc paths are latency-bound, and the stream-input WebSocket rejects
// v3/v4 outright, so only the fast models may serve them.
TEST(ElevenLabsModels, AdHocSpeechRefusesSlowAndNonStreamingModels) {
    EXPECT_TRUE(supportsAdHocSpeech("eleven_flash_v2_5"));
    EXPECT_TRUE(supportsAdHocSpeech("eleven_flash_v2"));
    EXPECT_TRUE(supportsAdHocSpeech("eleven_turbo_v2_5"));
    EXPECT_TRUE(supportsAdHocSpeech(kRecommendedAdHocModelId));

    EXPECT_FALSE(supportsAdHocSpeech("eleven_v4"));
    EXPECT_FALSE(supportsAdHocSpeech("eleven_v4_turbo"));
    EXPECT_FALSE(supportsAdHocSpeech("eleven_v3"));
    EXPECT_FALSE(supportsAdHocSpeech("eleven_multilingual_v2"));
    EXPECT_FALSE(supportsAdHocSpeech("eleven_monolingual_v1"));
    EXPECT_FALSE(supportsAdHocSpeech("eleven_multilingual_v1"));
}

TEST(ElevenLabsModels, TheDefaultExpressiveModelPerformsTags) {
    EXPECT_TRUE(supportsAudioTags(kDefaultExpressiveModelId));
}

namespace {

/// Exposes the protected setter, the same trick DialogCache_test uses.
class TestConfiguration : public Configuration {
  public:
    using Configuration::setExpressiveVoiceModel;
};

/// Swaps the global Configuration for the duration of a test.
class ExpressiveModelTest : public ::testing::Test {
  protected:
    void TearDown() override { config = saved_; }
    std::shared_ptr<Configuration> saved_ = config;
};

} // namespace

// The test binary almost never stands up a Configuration, and computeCacheKey()
// reaches for the model from a couple of dozen tests. A null config has to mean
// "the built-in default", not a crash (issue #244).
TEST_F(ExpressiveModelTest, FallsBackToTheDefaultWithNoConfiguration) {
    config.reset();
    EXPECT_EQ(expressiveModelId(), std::string(kDefaultExpressiveModelId));
}

// The whole point of #244: switch models without a recompile.
TEST_F(ExpressiveModelTest, ReadsTheConfiguredModel) {
    auto testConfig = std::make_shared<TestConfiguration>();
    testConfig->setExpressiveVoiceModel("eleven_v4_turbo");
    config = testConfig;
    EXPECT_EQ(expressiveModelId(), "eleven_v4_turbo");
}

// An unconfigured Configuration still carries the built-in default, so a
// server started with no config file renders dialog exactly as before.
TEST_F(ExpressiveModelTest, AnUnsetConfigurationStillHoldsTheDefault) {
    config = std::make_shared<TestConfiguration>();
    EXPECT_EQ(expressiveModelId(), std::string(kDefaultExpressiveModelId));
}

namespace {

// One character every 50 ms, the way ElevenLabs alignment arrives.
std::vector<TextToViseme::CharTiming> evenlyTimed(const std::string &text) {
    std::vector<TextToViseme::CharTiming> chars;
    for (std::size_t i = 0; i < text.size(); ++i) {
        chars.push_back({text[i], static_cast<double>(i) * 50.0, 50.0});
    }
    return chars;
}

} // namespace

// A tag comes back in the alignment with real timings ("[laughs]" spans the
// laugh), but it's performed, not spoken: the mouth must rest, not shape "laughs".
TEST(ElevenLabsModels, LipSyncRestsThroughAnAudioTag) {
    const std::string text = "Hello [laughs] there";
    const auto chars = evenlyTimed(text);
    const double tagStart = static_cast<double>(text.find('[')) * 0.05;
    const double tagEnd = static_cast<double>(text.find(']') + 1) * 0.05;

    TextToViseme viseme;
    const auto cues = viseme.charTimingsToMouthCues(chars);
    ASSERT_FALSE(cues.empty());

    bool restCoversTag = false;
    for (const auto &cue : cues) {
        const bool overlapsTag = cue.start < tagEnd && cue.end > tagStart;
        if (overlapsTag) {
            EXPECT_EQ(cue.value, "X") << "mouthed during the tag at " << cue.start << ".." << cue.end;
        }
        if (cue.value == "X" && cue.start <= tagStart && cue.end >= tagEnd) {
            restCoversTag = true;
        }
    }
    EXPECT_TRUE(restCoversTag) << "the tag's span should become a rest gap";

    // The words on either side still get mouthed.
    EXPECT_LT(cues.front().start, tagStart);
    EXPECT_GT(cues.back().end, tagEnd);
}

// Without tags the cues are unchanged: same text minus the tag mouths the same.
TEST(ElevenLabsModels, LipSyncIgnoresStrayClosingBrackets) {
    TextToViseme viseme;
    const auto plain = viseme.charTimingsToMouthCues(evenlyTimed("Hello there"));
    const auto stray = viseme.charTimingsToMouthCues(evenlyTimed("Hello] there"));
    ASSERT_FALSE(stray.empty());
    EXPECT_EQ(plain.size(), stray.size());
}

} // namespace creatures::voice
