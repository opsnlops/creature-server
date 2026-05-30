#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/DialogClient.h"
#include "server/voice/DialogPipeline.h"

using creatures::voice::chunkTurns;
using creatures::voice::DialogInput;

namespace {

DialogInput turn(std::string voice, std::string text) { return {std::move(voice), std::move(text)}; }

} // namespace

TEST(DialogPipelineChunkTurns, RejectsEmptyTurns) {
    const std::vector<DialogInput> empty;
    auto r = chunkTurns(empty);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogPipelineChunkTurns, RejectsZeroMaxChars) {
    const std::vector<DialogInput> turns{turn("v", "hi")};
    auto r = chunkTurns(turns, 0);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogPipelineChunkTurns, SingleTurnUnderCapIsOneChunk) {
    const std::vector<DialogInput> turns{turn("v", "hello world")};
    auto r = chunkTurns(turns, 100);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].size(), 1u);
    EXPECT_EQ(chunks[0][0].text, "hello world");
}

TEST(DialogPipelineChunkTurns, TwoTurnsFittingUnderCapStayInOneChunk) {
    // 5 + 5 = 10 chars, cap = 20.
    const std::vector<DialogInput> turns{turn("v1", "hello"), turn("v2", "world")};
    auto r = chunkTurns(turns, 20);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].size(), 2u);
}

TEST(DialogPipelineChunkTurns, SplitsAtTurnBoundaryWhenSecondPushesOverCap) {
    // 6 + 8 chars, cap = 10. First fits alone (6 ≤ 10); second alone would
    // fit (8 ≤ 10); together they'd be 14 > 10, so split between.
    const std::vector<DialogInput> turns{turn("v1", "abcdef"), turn("v2", "ghijklmn")};
    auto r = chunkTurns(turns, 10);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].size(), 1u);
    EXPECT_EQ(chunks[0][0].text, "abcdef");
    EXPECT_EQ(chunks[1].size(), 1u);
    EXPECT_EQ(chunks[1][0].text, "ghijklmn");
}

TEST(DialogPipelineChunkTurns, PacksMultipleSmallTurnsThenSplits) {
    // 3 turns of 4 chars each (12 total), cap = 8.
    // After turn 1 (4): current = 4. Turn 2 would push to 8 ≤ cap → stays.
    // After turn 2 (4+4=8): turn 3 would push to 12 > cap → split.
    const std::vector<DialogInput> turns{turn("v", "aaaa"), turn("v", "bbbb"), turn("v", "cccc")};
    auto r = chunkTurns(turns, 8);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 2u);
    EXPECT_EQ(chunks[0].size(), 2u);
    EXPECT_EQ(chunks[1].size(), 1u);
}

TEST(DialogPipelineChunkTurns, RejectsSingleTurnOverCap) {
    // A single turn longer than the cap can't be split (joint generation
    // is the whole point), so this fails fast rather than letting it hit
    // the ElevenLabs API only to come back with HTTP 400.
    const std::vector<DialogInput> turns{turn("v", "this turn is way too long to fit in the cap")};
    auto r = chunkTurns(turns, 10);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST(DialogPipelineChunkTurns, TurnExactlyAtCapFits) {
    const std::vector<DialogInput> turns{turn("v", "0123456789")};
    auto r = chunkTurns(turns, 10);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 1u);
    EXPECT_EQ(chunks[0].size(), 1u);
}

TEST(DialogPipelineChunkTurns, PreservesTurnOrderAndVoiceIds) {
    const std::vector<DialogInput> turns{turn("voice-A", "one"), turn("voice-B", "two"), turn("voice-A", "three"),
                                         turn("voice-B", "four")};
    auto r = chunkTurns(turns, 100);
    ASSERT_TRUE(r.isSuccess());
    const auto chunks = r.getValue().value();
    ASSERT_EQ(chunks.size(), 1u);
    ASSERT_EQ(chunks[0].size(), 4u);
    EXPECT_EQ(chunks[0][0].voiceId, "voice-A");
    EXPECT_EQ(chunks[0][1].voiceId, "voice-B");
    EXPECT_EQ(chunks[0][2].voiceId, "voice-A");
    EXPECT_EQ(chunks[0][3].voiceId, "voice-B");
    EXPECT_EQ(chunks[0][2].text, "three");
}
