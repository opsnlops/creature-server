
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/DialogPreviewAssembly.h"

namespace {

using creatures::voice::CachedGeneration;
using creatures::voice::mergeChunkGenerations;
using creatures::voice::normalizeCachedGenerationVoiceSegments;
using creatures::voice::normalizeVoiceSegmentIndices;

constexpr uint32_t kRate = 48000;
constexpr double kGap = 0.30;

/// A generation with `samples` of non-zero PCM, one word, and one voice segment.
CachedGeneration makeGen(std::size_t samples, double loss) {
    CachedGeneration gen;
    gen.audioPcm.assign(samples * 2, 0x7f);
    creatures::voice::ForcedAlignmentWord word;
    word.text = "hi";
    word.startSeconds = 0.01;
    word.endSeconds = 0.05;
    gen.forcedAlignment.words.push_back(word);
    creatures::voice::ForcedAlignmentChar ch;
    ch.text = "h";
    ch.startSeconds = 0.01;
    ch.endSeconds = 0.02;
    gen.forcedAlignment.characters.push_back(ch);
    gen.forcedAlignment.loss = loss;
    creatures::voice::DialogVoiceSegment seg;
    seg.voiceId = "voice";
    seg.characterStartIndex = 0;
    seg.characterEndIndex = 10;
    seg.dialogInputIndex = 0;
    gen.voiceSegments.push_back(seg);
    return gen;
}

TEST(DialogPreviewAssembly, ConcatenatesAudioWithSeamGaps) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.0), makeGen(4800, 0.0)}, {2, 3}, kRate, kGap);
    const std::size_t expectedSamples = 4800 + static_cast<std::size_t>(kGap * kRate) + 4800;
    EXPECT_EQ(expectedSamples * 2, merged.audioPcm.size());
}

TEST(DialogPreviewAssembly, OffsetsAlignmentTimesByChunkStart) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.0), makeGen(4800, 0.0)}, {2, 3}, kRate, kGap);
    ASSERT_EQ(2u, merged.forcedAlignment.words.size());
    // Chunk 0 is 0.1s long; chunk 1 starts after it + the 0.3s gap.
    EXPECT_DOUBLE_EQ(0.01, merged.forcedAlignment.words[0].startSeconds);
    EXPECT_DOUBLE_EQ(0.1 + kGap + 0.01, merged.forcedAlignment.words[1].startSeconds);
}

TEST(DialogPreviewAssembly, InsertsSyntheticSeamSpace) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.0), makeGen(4800, 0.0)}, {2, 3}, kRate, kGap);
    // chunk0 char, seam space, chunk1 char
    ASSERT_EQ(3u, merged.forcedAlignment.characters.size());
    const auto &seam = merged.forcedAlignment.characters[1];
    EXPECT_EQ(" ", seam.text);
    EXPECT_DOUBLE_EQ(0.1, seam.startSeconds);
    EXPECT_DOUBLE_EQ(0.1 + kGap, seam.endSeconds);
}

TEST(DialogPreviewAssembly, OffsetsSegmentIndices) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.0), makeGen(4800, 0.0)}, {2, 3}, kRate, kGap);
    ASSERT_EQ(2u, merged.voiceSegments.size());
    EXPECT_EQ(0u, merged.voiceSegments[0].characterStartIndex);
    EXPECT_EQ(0u, merged.voiceSegments[0].dialogInputIndex);
    // Chunk 0's char array ends at 10, +1 for the joining space.
    EXPECT_EQ(11u, merged.voiceSegments[1].characterStartIndex);
    EXPECT_EQ(21u, merged.voiceSegments[1].characterEndIndex);
    // Chunk 0 held 2 turns.
    EXPECT_EQ(2u, merged.voiceSegments[1].dialogInputIndex);
}

TEST(DialogPreviewAssembly, ReportsWorstChunkLoss) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.05), makeGen(4800, 0.2)}, {1, 1}, kRate, kGap);
    EXPECT_DOUBLE_EQ(0.2, merged.forcedAlignment.loss);
}

TEST(DialogPreviewAssembly, SingleChunkPassesThroughUnchanged) {
    const auto merged = mergeChunkGenerations({makeGen(4800, 0.1)}, {4}, kRate, kGap);
    EXPECT_EQ(4800u * 2, merged.audioPcm.size());
    ASSERT_EQ(1u, merged.forcedAlignment.words.size());
    EXPECT_DOUBLE_EQ(0.01, merged.forcedAlignment.words[0].startSeconds);
    EXPECT_EQ(0u, merged.voiceSegments[0].dialogInputIndex);
    EXPECT_FALSE(merged.generationId.empty());
}

TEST(DialogPreviewAssembly, NormalizesTaggedUtf8VoiceSegmentIndices) {
    const std::vector<creatures::voice::DialogInput> inputs{
        {"voice-A", "[excited] Beaky! BEAKY! I wanna tell you about Linux."},
        {"voice-B", "[annoyed] What, again? You always talk about Linux, Mango."},
        {"voice-A", "But Beaky, Linux 7.0 has been released! It’s seven now!"},
        {"voice-B",
         "[grumble] I don’t care, Mango. We live in a magical house on Whidbey Island! That’s way more important."},
    };
    std::vector<creatures::voice::DialogVoiceSegment> segments{
        {"voice-A", 0, 53, 0, 0.0, 0.0},
        {"voice-B", 53, 111, 1, 0.0, 0.0},
        {"voice-A", 111, 166, 2, 0.0, 0.0},
        {"voice-B", 166, 269, 3, 0.0, 0.0},
    };

    normalizeVoiceSegmentIndices(segments, inputs);

    EXPECT_EQ(segments[0].characterStartIndex, 0u);
    EXPECT_EQ(segments[0].characterEndIndex, 43u);
    EXPECT_EQ(segments[1].characterStartIndex, 44u);
    EXPECT_EQ(segments[1].characterEndIndex, 92u);
    EXPECT_EQ(segments[2].characterStartIndex, 93u);
    EXPECT_EQ(segments[2].characterEndIndex, 148u);
    EXPECT_EQ(segments[3].characterStartIndex, 149u);
    EXPECT_EQ(segments[3].characterEndIndex, 242u);
}

TEST(DialogPreviewAssembly, DoesNotNormalizeCachedSegmentsTwice) {
    const std::vector<creatures::voice::DialogInput> inputs{{"voice-A", "[excited] It’s seven now!"}};
    CachedGeneration generation;
    generation.voiceSegments.push_back({"voice-A", 0, 25, 0, 0.0, 0.0});

    normalizeCachedGenerationVoiceSegments(generation, inputs);
    const auto normalizedStart = generation.voiceSegments[0].characterStartIndex;
    const auto normalizedEnd = generation.voiceSegments[0].characterEndIndex;
    normalizeCachedGenerationVoiceSegments(generation, inputs);

    EXPECT_EQ(generation.voiceSegmentIndexSpace, creatures::voice::kVoiceSegmentIndexSpaceNormalized);
    EXPECT_EQ(generation.voiceSegments[0].characterStartIndex, normalizedStart);
    EXPECT_EQ(generation.voiceSegments[0].characterEndIndex, normalizedEnd);
}

} // namespace
