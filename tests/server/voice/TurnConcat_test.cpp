#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "server/voice/TurnConcat.h"

namespace creatures::voice {

namespace {

// Frames are opaque strings here; the helper never decodes them. Label each
// frame "<creature>:<part>:<index>" so a mix-up is obvious in a failure.
Track trackFor(const std::string &creatureId, int part, std::size_t frames) {
    Track track;
    track.creature_id = creatureId;
    for (std::size_t f = 0; f < frames; ++f) {
        track.frames.push_back(creatureId + ":" + std::to_string(part) + ":" + std::to_string(f));
    }
    return track;
}

// 48 kHz, 20 ms/frame → 960 samples per frame.
constexpr uint32_t kRate = 48000;
constexpr uint32_t kMsPerFrame = 20;

} // namespace

TEST(TurnConcat, ExactMultiplesConcatenateVerbatim) {
    std::vector<TurnTrackPart> parts;
    parts.push_back({{trackFor("A", 1, 3), trackFor("B", 1, 3)}, 3 * 960});
    parts.push_back({{trackFor("A", 2, 2), trackFor("B", 2, 2)}, 2 * 960});

    auto result = concatTurnTracks(parts, kMsPerFrame, kRate, "anim");

    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto value = result.getValue().value();
    EXPECT_EQ(value.totalFrames, 5u);
    ASSERT_EQ(value.tracks.size(), 2u);
    EXPECT_EQ(value.tracks[0].creature_id, "A");
    EXPECT_EQ(value.tracks[0].animation_id, "anim");
    EXPECT_FALSE(value.tracks[0].id.empty());
    ASSERT_EQ(value.tracks[0].frames.size(), 5u);
    EXPECT_EQ(value.tracks[0].frames[2], "A:1:2");
    EXPECT_EQ(value.tracks[0].frames[3], "A:2:0");
    EXPECT_EQ(value.tracks[1].frames[4], "B:2:1");
}

TEST(TurnConcat, RoundedUpPartsAreTrimmedSoTheTotalTracksTheAudio) {
    // Each part is 1.5 frames of audio, rendered as ceil() = 2 frames. Ten
    // such parts are 15 frames of audio, not 20: the drift the helper exists
    // to remove.
    std::vector<TurnTrackPart> parts;
    for (int p = 1; p <= 10; ++p) {
        parts.push_back({{trackFor("A", p, 2)}, 960 + 480});
    }

    auto result = concatTurnTracks(parts, kMsPerFrame, kRate, "anim");

    ASSERT_TRUE(result.isSuccess());
    const auto value = result.getValue().value();
    EXPECT_EQ(value.totalFrames, 15u);
    ASSERT_EQ(value.tracks[0].frames.size(), 15u);
    // Part 1 covers samples [0, 1440) → frames [0, 1): one frame.
    // Part 2 covers [1440, 2880) → frames [1, 3): two frames.
    EXPECT_EQ(value.tracks[0].frames[0], "A:1:0");
    EXPECT_EQ(value.tracks[0].frames[1], "A:2:0");
    EXPECT_EQ(value.tracks[0].frames[2], "A:2:1");
    EXPECT_EQ(value.tracks[0].frames[3], "A:3:0");
}

TEST(TurnConcat, AShortPartRepeatsItsLastFrame) {
    std::vector<TurnTrackPart> parts;
    parts.push_back({{trackFor("A", 1, 1)}, 3 * 960}); // rendered 1 frame for 3 frames of audio

    auto result = concatTurnTracks(parts, kMsPerFrame, kRate, "anim");

    ASSERT_TRUE(result.isSuccess());
    const auto value = result.getValue().value(); // getValue() is by value; never bind a reference through it
    const auto &frames = value.tracks[0].frames;
    ASSERT_EQ(frames.size(), 3u);
    EXPECT_EQ(frames[0], "A:1:0");
    EXPECT_EQ(frames[2], "A:1:0");
}

TEST(TurnConcat, TracksAreMatchedByCreatureNotByPosition) {
    std::vector<TurnTrackPart> parts;
    parts.push_back({{trackFor("A", 1, 1), trackFor("B", 1, 1)}, 960});
    parts.push_back({{trackFor("B", 2, 1), trackFor("A", 2, 1)}, 960}); // swapped

    auto result = concatTurnTracks(parts, kMsPerFrame, kRate, "anim");

    ASSERT_TRUE(result.isSuccess());
    const auto value = result.getValue().value();
    EXPECT_EQ(value.tracks[0].frames[1], "A:2:0");
    EXPECT_EQ(value.tracks[1].frames[1], "B:2:0");
}

TEST(TurnConcat, RejectsBadInput) {
    EXPECT_FALSE(concatTurnTracks({}, kMsPerFrame, kRate, "anim").isSuccess());

    std::vector<TurnTrackPart> noTracks;
    noTracks.push_back({{}, 960});
    EXPECT_FALSE(concatTurnTracks(noTracks, kMsPerFrame, kRate, "anim").isSuccess());

    std::vector<TurnTrackPart> missingCreature;
    missingCreature.push_back({{trackFor("A", 1, 1), trackFor("B", 1, 1)}, 960});
    missingCreature.push_back({{trackFor("A", 2, 1)}, 960});
    EXPECT_FALSE(concatTurnTracks(missingCreature, kMsPerFrame, kRate, "anim").isSuccess());

    std::vector<TurnTrackPart> fine;
    fine.push_back({{trackFor("A", 1, 1)}, 960});
    EXPECT_FALSE(concatTurnTracks(fine, 0, kRate, "anim").isSuccess());
    EXPECT_FALSE(concatTurnTracks(fine, kMsPerFrame, 0, "anim").isSuccess());
}

} // namespace creatures::voice
