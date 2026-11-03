#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/voice/StreamingTTSClient.h"

namespace creatures::voice {

// Both payloads below are the real shapes the two sockets send, captured live
// while working out issue #248. They differ only in spelling, which is exactly
// why this is worth a test: get it wrong and you receive a perfectly
// well-formed audio stream with no lip sync whatsoever.

namespace {

/// text-to-dialogue: snake_case.
nlohmann::json dialogueAlignment() {
    return nlohmann::json{
        {"chars", {"H", "e", "l"}}, {"char_start_times_ms", {0, 192, 224}}, {"char_durations_ms", {192, 32, 32}}};
}

/// text-to-speech: camelCase.
nlohmann::json ttsAlignment() {
    return nlohmann::json{
        {"chars", {"H", "e", "l"}}, {"charStartTimesMs", {0, 192, 224}}, {"charDurationsMs", {192, 32, 32}}};
}

} // namespace

TEST(DialogueWire, EachDialectHasItsOwnSpellings) {
    EXPECT_STREQ(wireKeys(WireDialect::Dialogue).final, "is_final_audio_for_turn");
    EXPECT_STREQ(wireKeys(WireDialect::Dialogue).starts, "char_start_times_ms");
    EXPECT_STREQ(wireKeys(WireDialect::Dialogue).durations, "char_durations_ms");

    EXPECT_STREQ(wireKeys(WireDialect::TextToSpeech).final, "isFinal");
    EXPECT_STREQ(wireKeys(WireDialect::TextToSpeech).starts, "charStartTimesMs");
    EXPECT_STREQ(wireKeys(WireDialect::TextToSpeech).durations, "charDurationsMs");
}

TEST(DialogueWire, ReadsTheDialogueSocketAlignment) {
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(dialogueAlignment(), WireDialect::Dialogue, timings, text, offset), 3u);
    ASSERT_EQ(timings.size(), 3u);
    EXPECT_EQ(text, "Hel");
    EXPECT_EQ(timings[0].character, 'H');
    EXPECT_DOUBLE_EQ(timings[0].startTimeMs, 0.0);
    EXPECT_DOUBLE_EQ(timings[0].durationMs, 192.0);
    EXPECT_DOUBLE_EQ(timings[2].startTimeMs, 224.0);
}

TEST(DialogueWire, ReadsTheTextToSpeechAlignment) {
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(ttsAlignment(), WireDialect::TextToSpeech, timings, text, offset), 3u);
    ASSERT_EQ(timings.size(), 3u);
    EXPECT_EQ(text, "Hel");
    EXPECT_DOUBLE_EQ(timings[2].startTimeMs, 224.0);
}

// The failure this guards against: a dialect mix-up is silent. Nothing throws,
// nothing logs an error, and the mouth simply never moves.
TEST(DialogueWire, TheWrongDialectTakesNothingRatherThanGuessing) {
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(dialogueAlignment(), WireDialect::TextToSpeech, timings, text, offset), 0u);
    EXPECT_EQ(appendAlignmentChunk(ttsAlignment(), WireDialect::Dialogue, timings, text, offset), 0u);
    EXPECT_TRUE(timings.empty());
    EXPECT_TRUE(text.empty());
}

TEST(DialogueWire, AppendsAcrossChunksBecauseAlignmentArrivesInPieces) {
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    appendAlignmentChunk(dialogueAlignment(), WireDialect::Dialogue, timings, text, offset);
    appendAlignmentChunk(dialogueAlignment(), WireDialect::Dialogue, timings, text, offset);
    EXPECT_EQ(timings.size(), 6u);
    EXPECT_EQ(text, "HelHel");
}

// Ragged arrays must not walk off the end of the shortest one.
TEST(DialogueWire, StopsAtTheShortestArray) {
    nlohmann::json ragged{
        {"chars", {"a", "b", "c"}}, {"char_start_times_ms", {0, 10}}, {"char_durations_ms", {10, 10, 10, 10}}};
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(ragged, WireDialect::Dialogue, timings, text, offset), 2u);
    EXPECT_EQ(text, "ab");
}

TEST(DialogueWire, AnEmptyCharacterBecomesASpace) {
    nlohmann::json withEmpty{{"chars", {""}}, {"char_start_times_ms", {0}}, {"char_durations_ms", {10}}};
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(withEmpty, WireDialect::Dialogue, timings, text, offset), 1u);
    EXPECT_EQ(text, " ");
}

TEST(DialogueWire, NonObjectsAndMissingFieldsAreRefusedQuietly) {
    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    EXPECT_EQ(appendAlignmentChunk(nlohmann::json::array(), WireDialect::Dialogue, timings, text, offset), 0u);
    EXPECT_EQ(appendAlignmentChunk(nlohmann::json{{"chars", {"a"}}}, WireDialect::Dialogue, timings, text, offset), 0u);
    EXPECT_TRUE(timings.empty());
}

// The bug that reached production (issue #252). The text-to-dialogue socket
// restarts every alignment chunk's times at zero; the text-to-speech socket
// reports them absolute. Concatenating the former stacks a whole utterance on
// top of itself, which shipped as birds whose beaks barely moved: the alignment
// claimed 11% of the audio was speech, and the mouths were open 11% of frames.
TEST(DialogueWire, DialogueChunksAreRelativeAndMustBeOffset) {
    // Two chunks, each starting from zero, as the socket really sends them.
    const auto chunkOne =
        nlohmann::json{{"chars", {"a", "b"}}, {"char_start_times_ms", {0, 40}}, {"char_durations_ms", {40, 40}}};
    const auto chunkTwo =
        nlohmann::json{{"chars", {"c", "d"}}, {"char_start_times_ms", {0, 40}}, {"char_durations_ms", {40, 40}}};

    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    ASSERT_EQ(appendAlignmentChunk(chunkOne, WireDialect::Dialogue, timings, text, offset), 2u);
    ASSERT_EQ(appendAlignmentChunk(chunkTwo, WireDialect::Dialogue, timings, text, offset), 2u);

    ASSERT_EQ(timings.size(), 4u);
    EXPECT_EQ(text, "abcd");
    // The second chunk is pushed past the first, not laid on top of it.
    EXPECT_DOUBLE_EQ(timings[0].startTimeMs, 0.0);
    EXPECT_DOUBLE_EQ(timings[1].startTimeMs, 40.0);
    EXPECT_DOUBLE_EQ(timings[2].startTimeMs, 80.0);
    EXPECT_DOUBLE_EQ(timings[3].startTimeMs, 120.0);

    for (std::size_t i = 1; i < timings.size(); ++i) {
        EXPECT_GE(timings[i].startTimeMs, timings[i - 1].startTimeMs) << "timings must not go backwards";
    }
}

// The text-to-speech socket's times are already absolute, so the offset must
// NOT be applied there — doing so would push its second chunk into the future.
TEST(DialogueWire, TextToSpeechChunksAreAbsoluteAndMustNotBeOffset) {
    const auto chunkOne =
        nlohmann::json{{"chars", {"a", "b"}}, {"charStartTimesMs", {0, 40}}, {"charDurationsMs", {40, 40}}};
    const auto chunkTwo =
        nlohmann::json{{"chars", {"c", "d"}}, {"charStartTimesMs", {80, 120}}, {"charDurationsMs", {40, 40}}};

    std::vector<TextToViseme::CharTiming> timings;
    std::string text;
    double offset = 0.0;
    ASSERT_EQ(appendAlignmentChunk(chunkOne, WireDialect::TextToSpeech, timings, text, offset), 2u);
    ASSERT_EQ(appendAlignmentChunk(chunkTwo, WireDialect::TextToSpeech, timings, text, offset), 2u);

    ASSERT_EQ(timings.size(), 4u);
    EXPECT_DOUBLE_EQ(timings[2].startTimeMs, 80.0) << "an absolute time was shifted";
    EXPECT_DOUBLE_EQ(timings[3].startTimeMs, 120.0);
    EXPECT_DOUBLE_EQ(offset, 0.0) << "the offset must stay untouched on an absolute socket";
}

TEST(DialogueWire, EachDialectDeclaresItsTimingConvention) {
    EXPECT_TRUE(wireKeys(WireDialect::Dialogue).chunkRelativeTimes);
    EXPECT_FALSE(wireKeys(WireDialect::TextToSpeech).chunkRelativeTimes);
}

} // namespace creatures::voice
