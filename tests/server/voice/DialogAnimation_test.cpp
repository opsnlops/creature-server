#include <cstdint>
#include <string>
#include <vector>

#include <base64.hpp>
#include <gtest/gtest.h>

#include "server/voice/DialogAnimation.h"
#include "util/ObservabilityManager.h"

#include "../TestGlobals.h"

namespace {

class DialogAnimationTest : public ::testing::Test {
  protected:
    void SetUp() override { creatures::observability = std::make_shared<creatures::ObservabilityManager>(); }
    void TearDown() override { creatures::observability.reset(); }
};

} // namespace

TEST_F(DialogAnimationTest, MusicTailExtendsAnimationWithIdleFrames) {
    creatures::voice::DialogAssembled assembled;
    assembled.sampleRate = 48000;
    assembled.totalSamples = 480; // 10 ms of spoken dialog.
    creatures::voice::DialogPerCreature voice;
    voice.voiceId = "voice-A";
    voice.pcm.assign(assembled.totalSamples, 1000);
    assembled.perCreature = {voice};

    creatures::voice::CreatureTrackInput input;
    input.voiceId = "voice-A";
    input.creatureId = "creature-A";
    input.creatureJson = {{"mouth_slot", 0}};
    input.baseFrames = {{0, 9}, {77, 8}};
    input.mouthBytes = {5};

    const auto result =
        creatures::voice::buildDialogAnimation(assembled, {input}, 10, "dialog.wav", "Music Tail", nullptr, {}, 4800);
    ASSERT_TRUE(result.isSuccess());
    ASSERT_EQ(result.getValue()->metadata.number_of_frames, 10u);
    ASSERT_EQ(result.getValue()->tracks.size(), 1u);
    ASSERT_EQ(result.getValue()->tracks[0].frames.size(), 10u);

    const auto lastFrame = base64::from_base64(result.getValue()->tracks[0].frames.back());
    ASSERT_EQ(lastFrame.size(), 2u);
    EXPECT_EQ(static_cast<uint8_t>(lastFrame[0]), 0u);
    EXPECT_EQ(static_cast<uint8_t>(lastFrame[1]), 9u);
}

TEST_F(DialogAnimationTest, RejectsAFrameCountThatCouldExhaustMemory) {
    creatures::voice::DialogAssembled assembled;
    assembled.sampleRate = 48000;
    assembled.totalSamples = 480;
    creatures::voice::DialogPerCreature voice;
    voice.voiceId = "voice-A";
    voice.pcm.assign(assembled.totalSamples, 1000);
    assembled.perCreature = {voice};

    creatures::voice::CreatureTrackInput input;
    input.voiceId = "voice-A";
    input.creatureId = "creature-A";
    input.creatureJson = {{"mouth_slot", 0}};
    input.baseFrames = {{0, 9}};

    const auto result = creatures::voice::buildDialogAnimation(assembled, {input}, 1, "dialog.wav", "Too Long", nullptr,
                                                               {}, 120001ULL * 48);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), creatures::ServerError::InvalidData);
}

TEST_F(DialogAnimationTest, RejectsBsonArrayOverheadBeforeBuildingTracks) {
    creatures::voice::DialogAssembled assembled;
    assembled.sampleRate = 48000;
    assembled.totalSamples = 480;

    std::vector<creatures::voice::CreatureTrackInput> inputs;
    for (int index = 0; index < 6; ++index) {
        const auto voiceId = "voice-" + std::to_string(index);
        creatures::voice::DialogPerCreature voice;
        voice.voiceId = voiceId;
        voice.pcm.assign(assembled.totalSamples, 1000);
        assembled.perCreature.push_back(std::move(voice));

        creatures::voice::CreatureTrackInput input;
        input.voiceId = voiceId;
        input.creatureId = "creature-" + std::to_string(index);
        input.creatureJson = {{"mouth_slot", 0}};
        input.baseFrames = {{0, 1, 2, 3, 4, 5, 6, 7}};
        inputs.push_back(std::move(input));
    }

    // At 120,000 frames the old base64-plus-JSON estimate was below 12 MiB,
    // while the BSON array keys and string envelopes make the document too
    // large. The preflight must reject before constructing those frame arrays.
    const auto result = creatures::voice::buildDialogAnimation(assembled, inputs, 1, "dialog.wav", "Too Large", nullptr,
                                                               {}, 120000ULL * 48);
    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), creatures::ServerError::InvalidData);
}
