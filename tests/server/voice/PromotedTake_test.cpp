#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/config/Configuration.h"
#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogWav.h"
#include "server/voice/IxmlWriter.h"
#include "server/voice/PromotedTake.h"

namespace creatures {
extern std::shared_ptr<Configuration> config;
}

namespace {

class TestConfiguration : public creatures::Configuration {
  public:
    using Configuration::setSoundFileLocation;
};

/// A promoted accepted-voice WAV lives under the permanent sound root as
/// dialog/voice/<slug>-<id>.wav; point the root at a temp dir for the test.
class PromotedTakeTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("promoted-take-test-" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(root_ / "dialog" / "voice");
        saved_ = creatures::config;
        auto cfg = std::make_shared<TestConfiguration>();
        cfg->setSoundFileLocation(root_.string());
        creatures::config = cfg;
    }
    void TearDown() override {
        creatures::config = saved_;
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }
    std::filesystem::path root_;
    std::shared_ptr<creatures::Configuration> saved_;
};

std::vector<int16_t> ramp(std::size_t n, int16_t step) {
    std::vector<int16_t> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<int16_t>((static_cast<int64_t>(i) * step) % 20000 - 10000);
    return out;
}

} // namespace

/// The render's own output round-trips: lanes come back sample-exact, and
/// the mouth cues + words the iXML carried become pre-baked lane data.
TEST_F(PromotedTakeTest, RebuildsAnAssembledTakeFromThePromotedWav) {
    creatures::voice::DialogAssembled original;
    original.sampleRate = 48000;
    original.totalSamples = 4800; // 0.1 s
    creatures::voice::DialogPerCreature beaky;
    beaky.voiceId = "voice-beaky";
    beaky.pcm = ramp(4800, 7);
    creatures::voice::DialogPerCreature mango;
    mango.voiceId = "voice-mango";
    mango.pcm = ramp(4800, 13);
    original.perCreature = {beaky, mango};

    creatures::voice::WavProvenance provenance;
    provenance.fileUid = "9e7bcff9-bb5d-4c8e-8eff-48513b8d7479";
    provenance.take = provenance.fileUid;
    provenance.tracks = {{1, "Beaky"}, {2, "Mango"}, {17, "BGM"}};
    provenance.lipsync = {{1, "Beaky", {{0.00, 0.04, "B"}, {0.04, 0.08, "X"}}}, {2, "Mango", {{0.05, 0.09, "D"}}}};
    provenance.wordAlignment = {{1, "Beaky", {{"hi", 0.00, 0.08}}}, {2, "Mango", {{"there", 0.05, 0.09}}}};

    const auto path = root_ / "dialog" / "voice" / "scene-9e7bcff9.wav";
    creatures::voice::VoiceChannelMap channels{{"voice-beaky", 1}, {"voice-mango", 2}};
    auto written = creatures::voice::writeDialogWav(original, channels, path, nullptr, &provenance);
    ASSERT_TRUE(written.isSuccess()) << written.getError()->getMessage();

    // The job's creature cache knows a third creature that isn't in this scene.
    const std::vector<creatures::voice::PromotedTakeLane> lanes = {
        {1, "voice-beaky"}, {2, "voice-mango"}, {3, "voice-kenny"}};
    auto loaded = creatures::voice::loadAssembledTakeFromPromotedFile("dialog/voice/scene-9e7bcff9.wav", lanes);
    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    const auto assembled = loaded.getValue().value();
    EXPECT_EQ(assembled.sampleRate, 48000u);
    EXPECT_EQ(assembled.totalSamples, 4800u);
    ASSERT_EQ(assembled.perCreature.size(), 2u);
    EXPECT_EQ(assembled.perCreature[0].voiceId, "voice-beaky");
    EXPECT_EQ(assembled.perCreature[0].pcm, beaky.pcm);
    EXPECT_EQ(assembled.perCreature[1].voiceId, "voice-mango");
    EXPECT_EQ(assembled.perCreature[1].pcm, mango.pcm);
    ASSERT_TRUE(assembled.perCreature[0].mouthCues.has_value());
    ASSERT_EQ(assembled.perCreature[0].mouthCues->size(), 2u);
    EXPECT_EQ(assembled.perCreature[0].mouthCues->at(1).value, "X");
    EXPECT_DOUBLE_EQ(assembled.perCreature[1].mouthCues->at(0).start, 0.05);
    ASSERT_EQ(assembled.perCreature[1].words.size(), 1u);
    EXPECT_EQ(assembled.perCreature[1].words[0].word, "there");
    EXPECT_TRUE(assembled.perCreature[0].mouth.empty());

    // mouthCuesFor prefers the pre-baked cues, so the render never re-derives.
    creatures::voice::TextToViseme viseme;
    EXPECT_EQ(creatures::voice::mouthCuesFor(assembled.perCreature[0], viseme).size(), 2u);
}

TEST_F(PromotedTakeTest, RefusesAFileWithNoTimingRatherThanRenderStillFaced) {
    creatures::voice::DialogAssembled original;
    original.sampleRate = 48000;
    original.totalSamples = 480;
    creatures::voice::DialogPerCreature beaky;
    beaky.voiceId = "voice-beaky";
    beaky.pcm = ramp(480, 3);
    original.perCreature = {beaky};
    creatures::voice::WavProvenance bare;
    bare.fileUid = "no-timing";
    bare.tracks = {{1, "Beaky"}};
    const auto path = root_ / "dialog" / "voice" / "bare.wav";
    ASSERT_TRUE(creatures::voice::writeDialogWav(original, {{"voice-beaky", 1}}, path, nullptr, &bare).isSuccess());

    auto loaded = creatures::voice::loadAssembledTakeFromPromotedFile("dialog/voice/bare.wav", {{1, "voice-beaky"}});
    ASSERT_FALSE(loaded.isSuccess());
    EXPECT_NE(loaded.getError()->getMessage().find("no timing on file"), std::string::npos);
    EXPECT_FALSE(
        creatures::voice::loadAssembledTakeFromPromotedFile("dialog/voice/missing.wav", {{1, "v"}}).isSuccess());
}

/// A prior render of the take reads back the same way — and its music tail
/// (silent creature lanes past the last spoken sample) is dropped so the
/// rebuilt take is the tightened timeline again.
TEST_F(PromotedTakeTest, ReadsAPriorRenderAndDropsItsMusicTail) {
    creatures::voice::DialogAssembled original;
    original.sampleRate = 48000;
    original.totalSamples = 2400;
    creatures::voice::DialogPerCreature beaky;
    beaky.voiceId = "voice-beaky";
    beaky.pcm = ramp(2400, 5);
    beaky.pcm.back() = 123; // make sure the last spoken sample is non-zero
    original.perCreature = {beaky};
    creatures::voice::WavProvenance provenance;
    provenance.fileUid = "render-1";
    provenance.generationIds = {"9e7bcff9-bb5d-4c8e-8eff-48513b8d7479"};
    provenance.tracks = {{1, "Beaky"}, {17, "BGM"}};
    provenance.lipsync = {{1, "Beaky", {{0.00, 0.05, "C"}}}};
    const std::vector<int16_t> music(9600, 1000); // 0.2 s of BGM: the render runs 4x longer than the speech
    const auto path = root_ / "dialog" / "render-with-music.wav";
    auto written = creatures::voice::writeDialogWav(original, {{"voice-beaky", 1}}, path, nullptr, &provenance, music);
    ASSERT_TRUE(written.isSuccess()) << written.getError()->getMessage();

    auto loaded =
        creatures::voice::loadAssembledTakeFromPromotedFile("dialog/render-with-music.wav", {{1, "voice-beaky"}});
    ASSERT_TRUE(loaded.isSuccess()) << loaded.getError()->getMessage();
    EXPECT_EQ(loaded.getValue()->totalSamples, 2400u);
    EXPECT_EQ(loaded.getValue()->perCreature[0].pcm, beaky.pcm);
}
