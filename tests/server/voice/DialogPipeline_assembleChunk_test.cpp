#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"

using creatures::voice::assembleChunk;
using creatures::voice::DialogInput;
using creatures::voice::DialogResult;
using creatures::voice::ForcedAlignmentChar;
using creatures::voice::ForcedAlignmentResult;
using creatures::voice::ForcedAlignmentWord;

// These tests synthesize fake DialogResult + ForcedAlignmentResult to exercise
// the slice + mouth-shift logic without hitting any API. The key invariants
// the pipeline guarantees:
//
//  1. One perCreature entry per unique voice; PCM length == totalSamples.
//  2. A voice's PCM is non-zero in its own turns' time slots and zero where
//     other voices speak (silent isolation — the whole point of Path A).
//  3. Char timings get shifted from the original (raw v3) timeline onto the
//     tightened output timeline so the mouth tracks the audio that plays.
//
// Test strategy: build a synthetic chunk where each turn's audio is a
// distinct constant value (turn 1 = 1000, turn 2 = 2000, turn 3 = 3000) and
// the FA timing is well-spaced, then assert the constants land in the
// expected voice + position.

namespace {

constexpr uint32_t kSR = 48000;

/// Pack an int16_t mono PCM buffer into a uint8_t byte vector (LE).
std::vector<uint8_t> packPcm(const std::vector<int16_t> &samples) {
    std::vector<uint8_t> bytes(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        bytes[2 * i] = static_cast<uint8_t>(samples[i] & 0xFF);
        bytes[2 * i + 1] = static_cast<uint8_t>((samples[i] >> 8) & 0xFF);
    }
    return bytes;
}

/// Build a single-word turn region in the PCM buffer at [startSec, endSec)
/// filled with `value`. Outside the region the buffer keeps its prior content.
void fillRange(std::vector<int16_t> &pcm, double startSec, double endSec, int16_t value) {
    const auto a = static_cast<std::size_t>(std::llround(startSec * kSR));
    const auto b = static_cast<std::size_t>(std::llround(endSec * kSR));
    for (std::size_t i = a; i < b && i < pcm.size(); ++i) {
        pcm[i] = value;
    }
}

ForcedAlignmentWord word(std::string text, double s, double e) { return {std::move(text), s, e}; }
ForcedAlignmentChar charT(std::string text, double s, double e) { return {std::move(text), s, e}; }

} // namespace

TEST(AssembleChunk, RejectsEmptyTurns) {
    DialogResult dr;
    dr.audioData.resize(100); // even-sized
    ForcedAlignmentResult fa;
    fa.words = {word("hi", 0.0, 0.1)};
    fa.characters = {charT("h", 0.0, 0.05), charT("i", 0.05, 0.1)};
    auto r = assembleChunk({}, dr, fa, kSR);
    ASSERT_FALSE(r.isSuccess());
}

TEST(AssembleChunk, RejectsOddAudioBytes) {
    DialogResult dr;
    dr.audioData.resize(7); // not 2-byte aligned
    ForcedAlignmentResult fa;
    fa.words = {word("hi", 0.0, 0.1)};
    fa.characters = {charT("h", 0.0, 0.05), charT("i", 0.05, 0.1)};
    std::vector<DialogInput> turns{{"v", "hi"}};
    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_FALSE(r.isSuccess());
}

TEST(AssembleChunk, RejectsZeroSampleRate) {
    DialogResult dr;
    dr.audioData.resize(100);
    ForcedAlignmentResult fa;
    fa.words = {word("hi", 0.0, 0.1)};
    fa.characters = {charT("h", 0.0, 0.05), charT("i", 0.05, 0.1)};
    std::vector<DialogInput> turns{{"v", "hi"}};
    auto r = assembleChunk(turns, dr, fa, 0);
    ASSERT_FALSE(r.isSuccess());
}

TEST(AssembleChunk, RejectsTurnWithNoWordsAfterStripping) {
    DialogResult dr;
    dr.audioData.resize(100);
    ForcedAlignmentResult fa;
    fa.words = {word("a", 0.0, 0.1)};
    fa.characters = {charT("a", 0.0, 0.1)};
    // Tag-only text strips to nothing → 0 words → rejected.
    std::vector<DialogInput> turns{{"v", "[whispering]"}};
    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_FALSE(r.isSuccess());
}

TEST(AssembleChunk, ProducesOnePerCreaturePerUniqueVoiceInFirstSeenOrder) {
    // Three turns, voices A → B → A. Expect 2 perCreature entries with A first.
    //   turn 1 "X" at 0.0-1.0  (voice A)
    //   turn 2 "Y" at 1.5-2.5  (voice B)
    //   turn 3 "Z" at 3.0-4.0  (voice A)
    //
    // PCM buffer: 5 seconds at 48 kHz = 240000 samples.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);
    fillRange(pcm, 3.0, 4.0, 3000);

    DialogResult dr;
    dr.audioData = packPcm(pcm);
    dr.audioFormat = "pcm_48000";

    ForcedAlignmentResult fa;
    fa.words = {word("X", 0.0, 1.0), word("Y", 1.5, 2.5), word("Z", 3.0, 4.0)};
    // Joined transcript "X Y Z" → 5 characters (turn chars + space separators).
    fa.characters = {charT("X", 0.0, 1.0), charT(" ", 1.0, 1.5), charT("Y", 1.5, 2.5), charT(" ", 2.5, 3.0),
                     charT("Z", 3.0, 4.0)};

    std::vector<DialogInput> turns{{"voice-A", "X"}, {"voice-B", "Y"}, {"voice-A", "Z"}};
    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess()) << (r.getError() ? r.getError().value().getMessage() : "");
    const auto out = r.getValue().value();

    ASSERT_EQ(out.perCreature.size(), 2u) << "two unique voices in the scene";
    EXPECT_EQ(out.perCreature[0].voiceId, "voice-A");
    EXPECT_EQ(out.perCreature[1].voiceId, "voice-B");
    EXPECT_EQ(out.sampleRate, kSR);
    EXPECT_GT(out.totalSamples, 0u);
    EXPECT_EQ(out.perCreature[0].pcm.size(), out.totalSamples);
    EXPECT_EQ(out.perCreature[1].pcm.size(), out.totalSamples);
}

TEST(AssembleChunk, PerCreaturePcmIsolatesEachVoiceToItsOwnTurnsOnly) {
    // Same fixture as above. Verify isolation: voice-A has non-zero samples
    // somewhere in its turns (turn 1 region and turn 3 region on the tightened
    // timeline), zero outside; voice-B has non-zero only in turn 2's region.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);
    fillRange(pcm, 3.0, 4.0, 3000);

    DialogResult dr;
    dr.audioData = packPcm(pcm);
    ForcedAlignmentResult fa;
    fa.words = {word("X", 0.0, 1.0), word("Y", 1.5, 2.5), word("Z", 3.0, 4.0)};
    fa.characters = {charT("X", 0.0, 1.0), charT(" ", 1.0, 1.5), charT("Y", 1.5, 2.5), charT(" ", 2.5, 3.0),
                     charT("Z", 3.0, 4.0)};
    std::vector<DialogInput> turns{{"voice-A", "X"}, {"voice-B", "Y"}, {"voice-A", "Z"}};

    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();

    auto countNonZero = [](const std::vector<int16_t> &v) {
        std::size_t n = 0;
        for (auto s : v) {
            if (s != 0)
                ++n;
        }
        return n;
    };

    const auto aNonZero = countNonZero(out.perCreature[0].pcm);
    const auto bNonZero = countNonZero(out.perCreature[1].pcm);

    // Voice A speaks in turns 1 + 3 (total ~2 seconds of source audio + onset/
    // release pads). Voice B speaks in turn 2 only (~1 second).
    EXPECT_GT(aNonZero, kSR) << "voice-A should have >1s of non-zero samples";
    EXPECT_GT(bNonZero, kSR / 2) << "voice-B should have >0.5s of non-zero samples";

    // ISOLATION: at any sample index, only one of A/B can be non-zero (or both
    // zero in the inter-turn gaps). They never overlap, because each slice
    // writes into a single voice's lane and the gaps are silence on both.
    for (std::size_t i = 0; i < out.totalSamples; ++i) {
        const bool aLoud = out.perCreature[0].pcm[i] != 0;
        const bool bLoud = out.perCreature[1].pcm[i] != 0;
        ASSERT_FALSE(aLoud && bLoud) << "both voices loud at sample " << i;
    }
}

TEST(AssembleChunk, MouthTimingsAreShiftedOntoTheTightenedTimeline) {
    // Each turn's first char should land near (but not before) the start of
    // the assembled audio. The first turn lands at ~0 ms (modulo a small
    // PAD_IN offset). Later turns get shifted to the running position on the
    // tightened timeline — NOT their original v3 timeline positions.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);

    DialogResult dr;
    dr.audioData = packPcm(pcm);
    ForcedAlignmentResult fa;
    fa.words = {word("X", 0.0, 1.0), word("Y", 1.5, 2.5)};
    fa.characters = {charT("X", 0.0, 1.0), charT(" ", 1.0, 1.5), charT("Y", 1.5, 2.5)};
    std::vector<DialogInput> turns{{"voice-A", "X"}, {"voice-B", "Y"}};

    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();

    ASSERT_EQ(out.perCreature.size(), 2u);
    ASSERT_FALSE(out.perCreature[0].mouth.empty()) << "voice-A has a mouth char";
    ASSERT_FALSE(out.perCreature[1].mouth.empty()) << "voice-B has a mouth char";

    // The TIGHTENED total is shorter than the original ~2.5s (the inter-turn
    // gap is squeezed down to INTER_TURN_GAP_SECS = 0.12s vs the original
    // 0.5s). So voice-B's char start on the output timeline is well below
    // 1.5s — the original v3 time.
    const auto &bFirstChar = out.perCreature[1].mouth.front();
    EXPECT_LT(bFirstChar.startTimeMs, 1500.0) << "tightened timeline should shrink the gap";
    EXPECT_GE(bFirstChar.startTimeMs, 0.0) << "still non-negative";

    // Voice-A's first char starts near 0 — its original start was 0.0, the
    // PAD_IN before the first slice happens to ALSO start at sample 0 (lo
    // clamps to 0 for the first turn), so the shift to the tightened timeline
    // is small or zero. Just sanity-check non-negative.
    EXPECT_GE(out.perCreature[0].mouth.front().startTimeMs, 0.0);
}

TEST(AssembleChunk, HandlesElevenLabsInterleavedWhitespaceWordTokens) {
    // Regression: live testing on 2026-05-30 revealed that ElevenLabs forced-
    // alignment returns words[] with inter-word whitespace tokens interleaved
    // (a 4-word turn comes back as 7 entries: "Beaky," " " "are" " " "you"
    // " " "awake?"). assembleChunk must filter those out before counting,
    // matching show.py's `if w["text"].strip() != ""` pre-filter. Without
    // the filter the wordCursor advances past spaces and consumes the wrong
    // word entries, mis-bracketing turn audio.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);
    DialogResult dr;
    dr.audioData = packPcm(pcm);

    // Two turns, 2 words each, with interleaved space tokens (mimics real
    // ElevenLabs FA output shape).
    ForcedAlignmentResult fa;
    fa.words = {
        word("Beaky,", 0.0, 0.5), word(" ", 0.5, 0.55), word("hello", 0.55, 1.0),  word(" ", 1.0, 1.5),
        word("Hi", 1.5, 2.0),     word(" ", 2.0, 2.05), word("Mango.", 2.05, 2.5),
    };
    fa.characters = {charT("B", 0.0, 0.1),  charT("e", 0.1, 0.2),  charT("a", 0.2, 0.3),  charT("k", 0.3, 0.4),
                     charT("y", 0.4, 0.45), charT(",", 0.45, 0.5), charT(" ", 0.5, 0.55), charT("h", 0.55, 0.6),
                     charT("e", 0.6, 0.65), charT("l", 0.65, 0.7), charT("l", 0.7, 0.75), charT("o", 0.75, 1.0),
                     charT(" ", 1.0, 1.5), // inter-turn separator
                     charT("H", 1.5, 1.7),  charT("i", 1.7, 2.0),  charT(" ", 2.0, 2.05), charT("M", 2.05, 2.1),
                     charT("a", 2.1, 2.15), charT("n", 2.15, 2.2), charT("g", 2.2, 2.4),  charT("o", 2.4, 2.45),
                     charT(".", 2.45, 2.5)};
    std::vector<DialogInput> turns{{"voice-A", "Beaky, hello"}, {"voice-B", "Hi Mango."}};

    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess()) << (r.getError() ? r.getError().value().getMessage() : "");
    const auto out = r.getValue().value();
    ASSERT_EQ(out.perCreature.size(), 2u);

    // Voice-A's mouth should include the full first turn's chars ("Beaky,
    // hello" = 12 chars including the comma + space). If the word filter were
    // missing, the cursor math would mis-bracket and we'd skip chars or
    // grab the inter-turn separator into the wrong turn.
    ASSERT_EQ(out.perCreature[0].mouth.size(), 12u);
    ASSERT_EQ(out.perCreature[1].mouth.size(), 9u); // "Hi Mango."
}

TEST(AssembleChunk, WordTimingsAreCapturedAndShiftedLikeTheMouthTimings) {
    // #56 Part 2: per-creature word timings must be captured from the FA words
    // and shifted onto the tightened timeline by the SAME amount as the chars,
    // so a word start lines up with its first char.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);
    DialogResult dr;
    dr.audioData = packPcm(pcm);
    ForcedAlignmentResult fa;
    fa.words = {word("X", 0.0, 1.0), word("Y", 1.5, 2.5)};
    fa.characters = {charT("X", 0.0, 1.0), charT(" ", 1.0, 1.5), charT("Y", 1.5, 2.5)};
    std::vector<DialogInput> turns{{"voice-A", "X"}, {"voice-B", "Y"}};

    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();
    ASSERT_EQ(out.perCreature.size(), 2u);

    ASSERT_EQ(out.perCreature[0].words.size(), 1u);
    EXPECT_EQ(out.perCreature[0].words[0].word, "X");
    ASSERT_EQ(out.perCreature[1].words.size(), 1u);
    EXPECT_EQ(out.perCreature[1].words[0].word, "Y");

    // Voice-B's word Y was at 1.5s originally; on the tightened timeline the gap
    // shrinks so it lands earlier — same as the char shift.
    EXPECT_LT(out.perCreature[1].words[0].start, 1.5);
    EXPECT_GE(out.perCreature[1].words[0].start, 0.0);
    EXPECT_LT(out.perCreature[1].words[0].start, out.perCreature[1].words[0].end);

    // The word's start (seconds → ms) must match its first char's shifted start,
    // proving both got the identical tightened-timeline shift.
    EXPECT_NEAR(out.perCreature[1].words[0].start * 1000.0, out.perCreature[1].mouth.front().startTimeMs, 1.0);
}

TEST(AssembleChunk, WordCaptureFiltersInterleavedWhitespaceTokens) {
    // Same interleaved-whitespace fixture as the char test: word capture must
    // land only the real words, not the " " separator tokens.
    std::vector<int16_t> pcm(static_cast<std::size_t>(5.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    fillRange(pcm, 1.5, 2.5, 2000);
    DialogResult dr;
    dr.audioData = packPcm(pcm);
    ForcedAlignmentResult fa;
    fa.words = {
        word("Beaky,", 0.0, 0.5), word(" ", 0.5, 0.55), word("hello", 0.55, 1.0),  word(" ", 1.0, 1.5),
        word("Hi", 1.5, 2.0),     word(" ", 2.0, 2.05), word("Mango.", 2.05, 2.5),
    };
    fa.characters = {charT("B", 0.0, 0.1),  charT("e", 0.1, 0.2),  charT("a", 0.2, 0.3),  charT("k", 0.3, 0.4),
                     charT("y", 0.4, 0.45), charT(",", 0.45, 0.5), charT(" ", 0.5, 0.55), charT("h", 0.55, 0.6),
                     charT("e", 0.6, 0.65), charT("l", 0.65, 0.7), charT("l", 0.7, 0.75), charT("o", 0.75, 1.0),
                     charT(" ", 1.0, 1.5),  charT("H", 1.5, 1.7),  charT("i", 1.7, 2.0),  charT(" ", 2.0, 2.05),
                     charT("M", 2.05, 2.1), charT("a", 2.1, 2.15), charT("n", 2.15, 2.2), charT("g", 2.2, 2.4),
                     charT("o", 2.4, 2.45), charT(".", 2.45, 2.5)};
    std::vector<DialogInput> turns{{"voice-A", "Beaky, hello"}, {"voice-B", "Hi Mango."}};

    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();
    ASSERT_EQ(out.perCreature.size(), 2u);

    ASSERT_EQ(out.perCreature[0].words.size(), 2u);
    EXPECT_EQ(out.perCreature[0].words[0].word, "Beaky,");
    EXPECT_EQ(out.perCreature[0].words[1].word, "hello");
    ASSERT_EQ(out.perCreature[1].words.size(), 2u);
    EXPECT_EQ(out.perCreature[1].words[0].word, "Hi");
    EXPECT_EQ(out.perCreature[1].words[1].word, "Mango.");
}

TEST(AssembleChunk, FailsCleanlyWhenForcedAlignmentRunsOutOfWords) {
    std::vector<int16_t> pcm(static_cast<std::size_t>(2.0 * kSR), 0);
    fillRange(pcm, 0.0, 1.0, 1000);
    DialogResult dr;
    dr.audioData = packPcm(pcm);

    ForcedAlignmentResult fa;
    fa.words = {word("only-one-word", 0.0, 1.0)};
    fa.characters = {charT("o", 0.0, 1.0)};
    // Two turns but FA only has one word total → must fail.
    std::vector<DialogInput> turns{{"v", "first"}, {"v", "second"}};
    auto r = assembleChunk(turns, dr, fa, kSR);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}
