#include <cstdint>
#include <string>
#include <vector>

#include <base64.hpp>
#include <gtest/gtest.h>

#include "server/voice/SpeechTrackBuilder.h"

namespace creatures::voice {

namespace {

// Decode a base64-encoded track frame back into bytes. Used in expectations
// to inspect what the builder actually emitted (which mouth byte landed
// where, which base frame got cycled in, etc.).
std::vector<uint8_t> decode(const std::string &encoded) {
    const std::string raw = base64::from_base64(encoded);
    return std::vector<uint8_t>(raw.begin(), raw.end());
}

// Three distinct 6-byte base frames so we can tell which one cycled in.
const std::vector<std::vector<uint8_t>> &baseFramesABC() {
    static const std::vector<std::vector<uint8_t>> kFrames{
        {0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA}, // A
        {0xBB, 0xBB, 0xBB, 0xBB, 0xBB, 0xBB}, // B
        {0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}, // C
    };
    return kFrames;
}

SpeechTrackInput baseInput(std::span<const uint8_t> mouthBytes, std::size_t mouthSlot, std::size_t totalFrames) {
    SpeechTrackInput input;
    input.baseFrames = baseFramesABC();
    input.mouthBytes = mouthBytes;
    input.mouthSlot = mouthSlot;
    input.totalFrames = totalFrames;
    input.creatureId = "test-creature";
    input.animationId = "test-animation";
    return input;
}

} // namespace

// =============================================================================
// The Beaky-chest regression. mouth_slot=7 on width-6 base frames must NOT
// crash. Mouth byte is silently dropped; body frames cycle as normal. This
// would have caught the 3.14.3 bug before it shipped.
// =============================================================================
TEST(SpeechTrackBuilder, MouthSlotPastFrameWidthDropsMouthByteAndDoesNotCrash) {
    std::vector<uint8_t> mouth = {0x05, 0x05, 0x05};
    auto input = baseInput(mouth, /*mouthSlot=*/7, /*totalFrames=*/3);

    auto result = buildSpeechTrack(input);

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    EXPECT_FALSE(result.getValue()->mouthSlotInRange);
    ASSERT_EQ(result.getValue()->track.frames.size(), 3u);

    // Body frames cycled normally (A, B, C), mouth byte NOT written.
    const auto f0 = decode(result.getValue()->track.frames[0]);
    EXPECT_EQ(f0, baseFramesABC()[0]);
    const auto f2 = decode(result.getValue()->track.frames[2]);
    EXPECT_EQ(f2, baseFramesABC()[2]);
}

// =============================================================================
// Simple cycling (ad-hoc shape). Base frames cycle; mouth slot in range but
// all mouth bytes are 0 → frames look unchanged.
// =============================================================================
TEST(SpeechTrackBuilder, SimpleCyclingNoMouthActivity) {
    std::vector<uint8_t> mouth(7, 0);
    auto input = baseInput(mouth, /*mouthSlot=*/3, /*totalFrames=*/7);

    auto result = buildSpeechTrack(input);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_TRUE(result.getValue()->mouthSlotInRange);
    EXPECT_EQ(result.getValue()->speakingFrameCount, 7u);
    ASSERT_EQ(result.getValue()->track.frames.size(), 7u);

    // Cycle: A B C A B C A — verify by inspecting the non-mouth slots.
    // The mouth slot (3) gets the mouth byte written (0 here) on every frame
    // — this matches the production behavior across all three call sites.
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[0], 0xAA); // A
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[0], 0xBB); // B
    EXPECT_EQ(decode(result.getValue()->track.frames[2])[0], 0xCC); // C
    EXPECT_EQ(decode(result.getValue()->track.frames[3])[0], 0xAA); // A again
    EXPECT_EQ(decode(result.getValue()->track.frames[6])[0], 0xAA); // A
    // Mouth slot got the mouth byte (0) written on every frame.
    for (std::size_t f = 0; f < 7; ++f) {
        EXPECT_EQ(decode(result.getValue()->track.frames[f])[3], 0x00) << "frame " << f;
    }
}

// =============================================================================
// Mouth byte lands at the right slot for each frame.
// =============================================================================
TEST(SpeechTrackBuilder, MouthByteLandsAtMouthSlot) {
    std::vector<uint8_t> mouth = {0x05, 0x00, 0x09};
    auto input = baseInput(mouth, /*mouthSlot=*/2, /*totalFrames=*/3);

    auto result = buildSpeechTrack(input);

    ASSERT_TRUE(result.isSuccess());
    const auto f0 = decode(result.getValue()->track.frames[0]);
    const auto f1 = decode(result.getValue()->track.frames[1]);
    const auto f2 = decode(result.getValue()->track.frames[2]);
    EXPECT_EQ(f0[2], 0x05);
    EXPECT_EQ(f1[2], 0x00);
    EXPECT_EQ(f2[2], 0x09);
    // Other slots stay as the base frame.
    EXPECT_EQ(f0[0], 0xAA);
    EXPECT_EQ(f2[5], 0xCC);
}

// =============================================================================
// Offset continuity (streaming shape). startOffset=2 → cycle starts at C, and
// endOffset reports where to pick up next time.
// =============================================================================
TEST(SpeechTrackBuilder, OffsetContinuityProducesCorrectStartAndEnd) {
    std::vector<uint8_t> mouth(4, 0);
    // Use mouth_slot=5 so the body-cycle bytes at slot 0 stay readable.
    auto input = baseInput(mouth, /*mouthSlot=*/5, /*totalFrames=*/4);

    SpeechTrackOptions options;
    options.startOffset = 2;
    auto result = buildSpeechTrack(input, options);

    ASSERT_TRUE(result.isSuccess());
    // Cycle starting at C: C A B C — verify by slot 0 (the non-mouth byte).
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[0], 0xCC); // C
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[0], 0xAA); // A
    EXPECT_EQ(decode(result.getValue()->track.frames[2])[0], 0xBB); // B
    EXPECT_EQ(decode(result.getValue()->track.frames[3])[0], 0xCC); // C

    // Next sentence picks up at (2 + 4) % 3 = 0
    EXPECT_EQ(result.getValue()->endOffset, 0u);
}

// =============================================================================
// Dialog idle mode. Speaking frames advance the body counter; silent frames
// freeze on baseFrames[0]. No tail extension.
// =============================================================================
TEST(SpeechTrackBuilder, DialogIdleModeFreezesSilentFramesOnBaseFrameZero) {
    // mouthBytes: speaking, speaking, silent, speaking, silent
    std::vector<uint8_t> mouth = {0x05, 0x05, 0x00, 0x05, 0x00};
    auto input = baseInput(mouth, /*mouthSlot=*/1, /*totalFrames=*/5);

    SpeechTrackOptions options;
    options.dialogIdleMode = true;
    options.bodyTailFrames = 0;
    auto result = buildSpeechTrack(input, options);

    ASSERT_TRUE(result.isSuccess());
    // Frame 0: speaking → A (counter=0); mouth=5
    // Frame 1: speaking → B (counter=1); mouth=5
    // Frame 2: silent  → A (idle)
    // Frame 3: speaking → C (counter=2); mouth=5
    // Frame 4: silent  → A (idle)
    EXPECT_EQ(result.getValue()->speakingFrameCount, 3u);
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[0], 0xAA); // A
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[0], 0xBB); // B
    EXPECT_EQ(decode(result.getValue()->track.frames[2])[0], 0xAA); // idle = A
    EXPECT_EQ(decode(result.getValue()->track.frames[3])[0], 0xCC); // C
    EXPECT_EQ(decode(result.getValue()->track.frames[4])[0], 0xAA); // idle = A

    // Mouth byte only on speaking frames.
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[1], 0x05);
    EXPECT_EQ(decode(result.getValue()->track.frames[2])[1], 0xAA); // idle frame untouched
    EXPECT_EQ(decode(result.getValue()->track.frames[3])[1], 0x05);
}

// =============================================================================
// Dialog idle mode WITH bodyTailFrames extension. The silent frame
// immediately after a speaking run still counts as "speaking" for body-cycle
// purposes — body doesn't snap to neutral the instant a turn ends.
// =============================================================================
TEST(SpeechTrackBuilder, DialogIdleModeBodyTailExtendsSpeakingRun) {
    // mouthBytes: speaking, silent, silent, silent — index 1 is within tail.
    std::vector<uint8_t> mouth = {0x05, 0x00, 0x00, 0x00};
    auto input = baseInput(mouth, /*mouthSlot=*/0, /*totalFrames=*/4);

    SpeechTrackOptions options;
    options.dialogIdleMode = true;
    options.bodyTailFrames = 1; // index 1 still counts as speaking
    auto result = buildSpeechTrack(input, options);

    ASSERT_TRUE(result.isSuccess());
    // Frame 0: speaking → A (counter=0); mouth byte 0x05 written at slot 0
    // Frame 1: tail-extended speaking → B (counter=1); mouth byte 0 written
    // Frame 2: silent (out of tail; 2 - 0 = 2 > 1) → idle = baseFrames[0] = A (NOT overwritten)
    // Frame 3: silent → idle = A (NOT overwritten)
    EXPECT_EQ(result.getValue()->speakingFrameCount, 2u);

    // Speaking frames have the mouth byte written at slot 0; check body via slot 1.
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[0], 0x05); // mouth byte 0x05
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[1], 0xAA); // body = A
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[0], 0x00); // mouth byte 0
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[1], 0xBB); // body = B

    // Idle frames freeze on baseFrames[0] — NOT overwritten with anything.
    EXPECT_EQ(decode(result.getValue()->track.frames[2]), baseFramesABC()[0]);
    EXPECT_EQ(decode(result.getValue()->track.frames[3]), baseFramesABC()[0]);
}

// =============================================================================
// Empty baseFrames → InvalidData with a helpful message.
// =============================================================================
TEST(SpeechTrackBuilder, EmptyBaseFramesIsInvalidData) {
    SpeechTrackInput input;
    input.baseFrames = {}; // explicitly empty
    std::vector<uint8_t> mouth = {0x05};
    input.mouthBytes = mouth;
    input.mouthSlot = 0;
    input.totalFrames = 1;
    input.creatureId = "test";
    input.animationId = "anim";

    auto result = buildSpeechTrack(input);

    ASSERT_FALSE(result.isSuccess());
    EXPECT_EQ(result.getError()->getCode(), ServerError::InvalidData);
}

// =============================================================================
// mouthBytes shorter than totalFrames → trailing frames just don't get a
// mouth byte written. In simple mode every frame is still "speaking" (body
// advances). No crash from indexing past the end.
// =============================================================================
TEST(SpeechTrackBuilder, MouthBytesShorterThanTotalFramesIsHandled) {
    // 3 mouth bytes for 5 target frames.
    std::vector<uint8_t> mouth = {0x05, 0x07, 0x09};
    auto input = baseInput(mouth, /*mouthSlot=*/2, /*totalFrames=*/5);

    auto result = buildSpeechTrack(input);

    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue()->speakingFrameCount, 5u);
    // First three frames have mouth bytes; last two don't (base byte preserved).
    EXPECT_EQ(decode(result.getValue()->track.frames[0])[2], 0x05);
    EXPECT_EQ(decode(result.getValue()->track.frames[1])[2], 0x07);
    EXPECT_EQ(decode(result.getValue()->track.frames[2])[2], 0x09);
    EXPECT_EQ(decode(result.getValue()->track.frames[3])[2], 0xAA); // base frame A's byte
    EXPECT_EQ(decode(result.getValue()->track.frames[4])[2], 0xBB); // base frame B's byte
}

// =============================================================================
// Cock-axis ownership (issue #144).
//
// The gaze layer holds a speaking creature's cock at a dead constant for the
// whole turn, which stripped Beaky's head_tilt from 82 distinct values down to
// 1. The loop keeps the tilt while speaking; gaze takes it while listening.
// =============================================================================
TEST(SpeechTrackBuilder, LoopKeepsTheCockAxisWhileSpeaking) {
    // Speaking throughout: mouth is voiced on every frame.
    std::vector<uint8_t> mouth(6, 0x05);
    auto input = baseInput(mouth, /*mouthSlot=*/4, /*totalFrames=*/6);

    // Gaze wants a hard 0x11 on slot 1 for every frame.
    std::vector<uint8_t> cock(6, 0x11);
    input.gazeCockBytes = cock;
    input.gazeCockSlot = 1;

    SpeechTrackOptions options;
    options.crossfadeFrames = 0; // isolate ownership from the handover easing

    auto result = buildSpeechTrack(input, options);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();

    // Every frame is a speaking frame, so the body loop's own tilt survives
    // untouched — A, B, C, A, B, C — rather than being flattened to 0x11.
    const std::vector<uint8_t> expected{0xAA, 0xBB, 0xCC, 0xAA, 0xBB, 0xCC};
    for (std::size_t f = 0; f < expected.size(); ++f) {
        EXPECT_EQ(decode(result.getValue()->track.frames[f])[1], expected[f]) << "frame " << f;
    }
}

TEST(SpeechTrackBuilder, GazeTakesTheCockAxisWhileListening) {
    // Silent throughout, so this creature is listening the whole time.
    std::vector<uint8_t> mouth(6, 0x00);
    auto input = baseInput(mouth, /*mouthSlot=*/4, /*totalFrames=*/6);

    std::vector<uint8_t> cock(6, 0x11);
    input.gazeCockBytes = cock;
    input.gazeCockSlot = 1;

    SpeechTrackOptions options;
    options.dialogIdleMode = true; // required for silent frames to be possible
    options.bodyTailFrames = 0;
    options.crossfadeFrames = 0;

    auto result = buildSpeechTrack(input, options);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();

    for (std::size_t f = 0; f < 6; ++f) {
        EXPECT_EQ(decode(result.getValue()->track.frames[f])[1], 0x11) << "frame " << f;
    }
}

TEST(SpeechTrackBuilder, CockHandoverIsEasedRatherThanSnapped) {
    // Speaks for the first half, listens for the second. The tilt must not jump
    // straight from the loop's value to the gaze value at the boundary.
    std::vector<uint8_t> mouth{0x05, 0x05, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00};
    auto input = baseInput(mouth, /*mouthSlot=*/4, /*totalFrames=*/8);

    std::vector<uint8_t> cock(8, 0xF0);
    input.gazeCockBytes = cock;
    input.gazeCockSlot = 1;

    SpeechTrackOptions options;
    options.dialogIdleMode = true;
    options.bodyTailFrames = 0;
    options.crossfadeFrames = 3;

    auto result = buildSpeechTrack(input, options);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();

    // Frame 3 is the first listening frame. Easing means it must not already
    // be sitting on the gaze value.
    const int atHandover = decode(result.getValue()->track.frames[3])[1];
    EXPECT_LT(atHandover, 0xF0) << "cock snapped straight to the gaze value";

    // ...but by the end of the fade it has arrived.
    EXPECT_EQ(decode(result.getValue()->track.frames[7])[1], 0xF0);
}

} // namespace creatures::voice
