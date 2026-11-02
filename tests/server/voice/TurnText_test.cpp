#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/TextToViseme.h"
#include "server/voice/TurnText.h"

// Both ends of issue #253, which is one bug with two halves.
//
// Going out: a streaming turn's text was always stripped of its audio tags,
// whichever socket rendered it, so eleven_v4_turbo over the text-to-dialogue
// socket never performed one. splitTurnText is that decision, named and
// testable; StreamingAdHocSession::addTurn is the only caller.
//
// Coming back: a model that performs a tag reports the tag's own characters in
// its alignment, with real timings spread across the sigh. Those characters
// must not reach the beak, or the bird mouths "sighs" through the sound of
// sighing. The guard lives in charTimingsToMouthCues; nothing exercised it on
// the streaming path until now.

namespace creatures::voice {
namespace {

/// Lay a string out as alignment: one entry per byte, each `msPerChar` long,
/// appended starting at `startMs`. Only the spans matter here, so evenly spaced
/// characters stand in for what the socket really sends.
void append(std::vector<TextToViseme::CharTiming> &into, const std::string &text, const double startMs,
            const double msPerChar) {
    double at = startMs;
    for (const char c : text) {
        into.push_back(TextToViseme::CharTiming{c, at, msPerChar});
        at += msPerChar;
    }
}

bool anyMovementWhere(const std::vector<RhubarbMouthCue> &cues, const std::function<bool(double)> &startsAt) {
    return std::any_of(cues.begin(), cues.end(),
                       [&](const RhubarbMouthCue &cue) { return cue.value != "X" && startsAt(cue.start); });
}

} // namespace

TEST(TurnTextSplit, TagsReachAProviderThatPerformsThem) {
    const auto split = splitTurnText("I don't believe it. [sighs] Fine, we'll try again.", true);

    // The whole point: what ElevenLabs is asked to say still has the cue in it.
    EXPECT_EQ(split.forProvider, "I don't believe it. [sighs] Fine, we'll try again.");
    // And the transcript, the animation title and the exchange provenance do
    // not, because nobody says "sighs".
    EXPECT_EQ(split.forRecord, "I don't believe it. Fine, we'll try again.");
}

TEST(TurnTextSplit, TagsAreStrippedForAProviderThatWouldReadThemAloud) {
    const auto split = splitTurnText("I don't believe it. [sighs] Fine, we'll try again.", false);

    // Unchanged from before #253: the fast models say "Sighs." out loud.
    EXPECT_EQ(split.forProvider, "I don't believe it. Fine, we'll try again.");
    EXPECT_EQ(split.forRecord, split.forProvider);
}

TEST(TurnTextSplit, UntaggedTextIsTheSameEitherWay) {
    const std::string plain = "Good morning, Beaky.";
    for (const bool performsTags : {false, true}) {
        const auto split = splitTurnText(plain, performsTags);
        EXPECT_EQ(split.forProvider, plain);
        EXPECT_EQ(split.forRecord, plain);
    }
}

TEST(TurnTextSplit, OnlyTheRecordIsNormalized) {
    const auto split = splitTurnText("  [whispering]  Beaky.\n\nWake   up.  ", true);

    // The line goes to the provider as written. Its whitespace is its own
    // business; only the record gets compared, excerpted and slugified.
    EXPECT_EQ(split.forProvider, "  [whispering]  Beaky.\n\nWake   up.  ");
    EXPECT_EQ(split.forRecord, "Beaky. Wake up.");
}

TEST(TurnTextSplit, ACueOnlyTurnLeavesNothingToRecord) {
    const auto split = splitTurnText("[sighs]", true);

    // addTurn rejects the chunk on exactly this being empty, which is what
    // keeps a turn of pure cue out of the transcript (#253, item 3).
    EXPECT_TRUE(split.forRecord.empty());
    EXPECT_EQ(split.forProvider, "[sighs]");
}

TEST(TaggedAlignmentMouthCues, TheMouthRestsThroughAPerformedTag) {
    // What a tag-performing model reports: the bracketed characters carry real
    // timings, stretched across the 1.4 s it spends sighing.
    std::vector<TextToViseme::CharTiming> timings;
    append(timings, "Hi. ", 0.0, 100.0);      // "Hi" ends at 0.2 s
    append(timings, "[sighs]", 400.0, 200.0); // the sigh runs 0.4 s - 1.8 s
    append(timings, " Go.", 1800.0, 100.0);   // "Go" runs 1.9 s - 2.1 s

    const TextToViseme viseme;
    const auto cues = viseme.charTimingsToMouthCues(timings);
    ASSERT_FALSE(cues.empty());

    for (const auto &cue : cues) {
        const bool overlapsTheSigh = cue.end > 0.4 + 1e-6 && cue.start < 1.8 - 1e-6;
        if (overlapsTheSigh) {
            EXPECT_EQ(cue.value, "X") << "cue '" << cue.value << "' at [" << cue.start << ", " << cue.end
                                      << "] moves the beak while the sigh is playing";
        }
    }

    // Rest everywhere is also wrong — that was #252. The words on both sides
    // of the tag still have to move the beak.
    EXPECT_TRUE(anyMovementWhere(cues, [](const double start) { return start < 0.4; })) << "nothing moves on \"Hi\"";
    EXPECT_TRUE(anyMovementWhere(cues, [](const double start) { return start >= 1.8; })) << "nothing moves on \"Go\"";

    // And what resumes lands on "Go", not wherever mouthing "sighs" would have
    // left the timeline.
    const auto resumed = std::find_if(cues.begin(), cues.end(),
                                      [](const RhubarbMouthCue &cue) { return cue.value != "X" && cue.start > 1.0; });
    ASSERT_NE(resumed, cues.end());
    EXPECT_NEAR(resumed->start, 1.9, 0.01);
}

TEST(TaggedAlignmentMouthCues, ASilentGapWhereTheTagPlayedAlsoRests) {
    // The other shape the wire could take: the tag absorbed, leaving only
    // spoken characters and a hole in the timeline where the sigh happened.
    // The result has to be the same closed beak, so neither wire behavior
    // needs a code change if ElevenLabs changes its mind.
    std::vector<TextToViseme::CharTiming> timings;
    append(timings, "Hi. ", 0.0, 100.0);
    append(timings, "Go.", 1800.0, 100.0);

    const TextToViseme viseme;
    const auto cues = viseme.charTimingsToMouthCues(timings);
    ASSERT_FALSE(cues.empty());

    for (const auto &cue : cues) {
        if (cue.end > 0.4 + 1e-6 && cue.start < 1.8 - 1e-6) {
            EXPECT_EQ(cue.value, "X") << "cue '" << cue.value << "' at [" << cue.start << ", " << cue.end
                                      << "] fills a gap that should be silent";
        }
    }
    EXPECT_TRUE(anyMovementWhere(cues, [](const double start) { return start >= 1.8; })) << "nothing moves on \"Go\"";
}

} // namespace creatures::voice
