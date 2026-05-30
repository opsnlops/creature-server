#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/DialogPipeline.h"

using creatures::voice::concatChunks;
using creatures::voice::DialogAssembled;
using creatures::voice::DialogPerCreature;

// concatChunks is pure data shuffling — no API calls, no spans. It needs to:
//  - union the voice sets across chunks (preserving first-seen order);
//  - zero-pad any voice that doesn't speak in a given chunk;
//  - insert a small inter-chunk silence gap;
//  - offset mouth timings by accumulated prior samples + gaps.

namespace {

constexpr uint32_t kSR = 48000;

DialogPerCreature voice(std::string id, std::size_t samples, int16_t fillValue, std::vector<double> mouthStartMs = {}) {
    DialogPerCreature pc;
    pc.voiceId = std::move(id);
    pc.pcm.assign(samples, fillValue);
    for (auto s : mouthStartMs) {
        creatures::voice::TextToViseme::CharTiming c{};
        c.character = 'x';
        c.startTimeMs = s;
        c.durationMs = 50.0;
        pc.mouth.push_back(c);
    }
    return pc;
}

DialogAssembled chunk(std::size_t samples, std::vector<DialogPerCreature> perCreature) {
    DialogAssembled a;
    a.sampleRate = kSR;
    a.totalSamples = samples;
    a.perCreature = std::move(perCreature);
    return a;
}

} // namespace

TEST(ConcatChunks, RejectsEmptyInput) {
    auto r = concatChunks({});
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(ConcatChunks, SingleChunkReturnedAsIs) {
    auto c = chunk(100, {voice("A", 100, 1000)});
    auto r = concatChunks({c});
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();
    EXPECT_EQ(out.totalSamples, 100u);
    ASSERT_EQ(out.perCreature.size(), 1u);
    EXPECT_EQ(out.perCreature[0].voiceId, "A");
}

TEST(ConcatChunks, RejectsMismatchedSampleRate) {
    auto c1 = chunk(100, {voice("A", 100, 1000)});
    auto c2 = chunk(100, {voice("A", 100, 1000)});
    c2.sampleRate = 44100; // mismatch
    auto r = concatChunks({c1, c2});
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(ConcatChunks, UnionsVoicesAcrossChunksInFirstSeenOrder) {
    // chunk 1: A, B
    // chunk 2: B, C
    // → output should have A, B, C in that order.
    auto c1 = chunk(100, {voice("A", 100, 1), voice("B", 100, 2)});
    auto c2 = chunk(100, {voice("B", 100, 3), voice("C", 100, 4)});
    auto r = concatChunks({c1, c2});
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();
    ASSERT_EQ(out.perCreature.size(), 3u);
    EXPECT_EQ(out.perCreature[0].voiceId, "A");
    EXPECT_EQ(out.perCreature[1].voiceId, "B");
    EXPECT_EQ(out.perCreature[2].voiceId, "C");
}

TEST(ConcatChunks, ZeroPadsAbsentVoicesForEachChunk) {
    // A speaks only in chunk 1; C speaks only in chunk 2. Both their lanes
    // should be full-length (== totalSamples) with zeros wherever they're not
    // present, and the chunk2 region of A / chunk1 region of C should be 0.
    const std::size_t s1 = 100;
    const std::size_t s2 = 200;
    auto c1 = chunk(s1, {voice("A", s1, 1000), voice("B", s1, 2000)});
    auto c2 = chunk(s2, {voice("B", s2, 3000), voice("C", s2, 4000)});
    auto r = concatChunks({c1, c2});
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();

    // Inter-chunk gap is INTER_CHUNK_GAP_SECS = 0.30s, computed in concatChunks.
    const std::size_t gap = static_cast<std::size_t>(0.30 * static_cast<double>(kSR) + 0.5); // round-to-nearest
    EXPECT_EQ(out.totalSamples, s1 + gap + s2);

    // For every output lane, length must equal totalSamples.
    for (const auto &pc : out.perCreature) {
        EXPECT_EQ(pc.pcm.size(), out.totalSamples);
    }

    // Find each voice's lane.
    auto laneOf = [&](const std::string &id) -> const DialogPerCreature * {
        for (const auto &pc : out.perCreature) {
            if (pc.voiceId == id)
                return &pc;
        }
        return nullptr;
    };
    const auto *a = laneOf("A");
    const auto *b = laneOf("B");
    const auto *c = laneOf("C");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(c, nullptr);

    // A: non-zero in chunk1 region, zero in the gap, zero throughout chunk2.
    EXPECT_EQ(a->pcm[0], 1000);
    EXPECT_EQ(a->pcm[s1 - 1], 1000);
    EXPECT_EQ(a->pcm[s1], 0) << "first gap sample is silent";
    EXPECT_EQ(a->pcm[s1 + gap], 0) << "first chunk2 sample for A is zero-padded";
    EXPECT_EQ(a->pcm[out.totalSamples - 1], 0);

    // C: zero throughout chunk1 + gap, non-zero in chunk2.
    EXPECT_EQ(c->pcm[0], 0);
    EXPECT_EQ(c->pcm[s1 - 1], 0);
    EXPECT_EQ(c->pcm[s1 + gap], 4000);
    EXPECT_EQ(c->pcm[out.totalSamples - 1], 4000);

    // B speaks in both — non-zero in both regions, zero in the gap.
    EXPECT_EQ(b->pcm[0], 2000);
    EXPECT_EQ(b->pcm[s1 - 1], 2000);
    EXPECT_EQ(b->pcm[s1], 0);
    EXPECT_EQ(b->pcm[s1 + gap], 3000);
}

TEST(ConcatChunks, MouthTimingsShiftByAccumulatedOffset) {
    // Chunk 1: voice-A with one mouth char at 50 ms.
    // Chunk 2: voice-A with one mouth char at 25 ms.
    // After concat: chunk1's char stays at 50 ms; chunk2's char shifts by
    // (s1 + gap)/SR * 1000 + 25.
    const std::size_t s1 = static_cast<std::size_t>(0.5 * kSR); // 0.5 s
    const std::size_t s2 = static_cast<std::size_t>(0.5 * kSR);
    auto c1 = chunk(s1, {voice("A", s1, 1, {50.0})});
    auto c2 = chunk(s2, {voice("A", s2, 1, {25.0})});
    auto r = concatChunks({c1, c2});
    ASSERT_TRUE(r.isSuccess());
    const auto out = r.getValue().value();
    ASSERT_EQ(out.perCreature.size(), 1u);
    ASSERT_EQ(out.perCreature[0].mouth.size(), 2u);

    const auto &m1 = out.perCreature[0].mouth[0];
    const auto &m2 = out.perCreature[0].mouth[1];
    EXPECT_DOUBLE_EQ(m1.startTimeMs, 50.0) << "chunk1's char unchanged";

    // Expected offset for chunk 2: (s1 + gap)/SR seconds in ms.
    const std::size_t gap = static_cast<std::size_t>(0.30 * static_cast<double>(kSR) + 0.5);
    const double expectedOffsetMs = static_cast<double>(s1 + gap) / static_cast<double>(kSR) * 1000.0;
    EXPECT_NEAR(m2.startTimeMs, expectedOffsetMs + 25.0, 0.1) << "chunk2's char shifted by prior length + gap";
}
