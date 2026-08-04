#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <set>

#include "server/voice/GazeTrack.h"

using namespace creatures::voice;

// ===========================================================================
// The four-creature fixture from docs/idle-and-stage-gaze-plan.md.
//
// This is Layout B — the "conversation V" — with the listener at the origin.
// The numbers in the plan were derived by hand; pinning them here is what
// proves the implementation matches the design rather than merely being
// self-consistent. A sign flip anywhere in the bearing math sends every head
// the wrong way and still looks plausible in code review, so these have to be
// checked against externally-computed values.
//
//   name   x      y      z      yaw     pan range    elevation range
//   A    -2.4   +0.10  -3.0    +35     +/-55        +/-30
//   B    -0.8   -0.30  -3.4    +15     +/-60 (inv)  +/-30
//   C    +0.8   -0.50  -3.4    -15     +/-50        +/-30
//   D    +2.4   +0.25  -3.0    -35     +/-45        +/-30
// ===========================================================================

namespace {

// Slots mirror Beaky/Crow's real input layout: head_tilt@0, head_height@1,
// neck_rotate@2. The point of the name-based config is that these differ per
// creature, so the tests use concrete numbers rather than assuming a layout.
ResolvedGazeAxis pan(float atMin, float atMax) { return ResolvedGazeAxis{2, atMin, atMax, 0.4f}; }
ResolvedGazeAxis elevation(float atMin, float atMax) { return ResolvedGazeAxis{1, atMin, atMax, 0.4f}; }
ResolvedGazeAxis cock(float atMin, float atMax) { return ResolvedGazeAxis{0, atMin, atMax, 0.4f}; }

GazeGeometry creatureA() {
    GazeGeometry g;
    g.creatureId = "A";
    g.x = -2.4f;
    g.y = 0.10f;
    g.z = -3.0f;
    g.yaw = 35.0f;
    g.pan = pan(-55.0f, 55.0f);
    g.elevation = elevation(-30.0f, 30.0f);
    g.cock = cock(-20.0f, 20.0f);
    return g;
}

GazeGeometry creatureB() {
    GazeGeometry g;
    g.creatureId = "B";
    g.x = -0.8f;
    g.y = -0.30f;
    g.z = -3.4f;
    g.yaw = 15.0f;
    // Inverted pan motor: degrees_at_0 > degrees_at_255.
    g.pan = pan(60.0f, -60.0f);
    g.elevation = elevation(-30.0f, 30.0f);
    g.cock = cock(-20.0f, 20.0f);
    return g;
}

GazeGeometry creatureC() {
    GazeGeometry g;
    g.creatureId = "C";
    g.x = 0.8f;
    g.y = -0.50f;
    g.z = -3.4f;
    g.yaw = -15.0f;
    g.pan = pan(-50.0f, 50.0f);
    g.elevation = elevation(-30.0f, 30.0f);
    g.cock = cock(-20.0f, 20.0f);
    return g;
}

GazeGeometry creatureD() {
    GazeGeometry g;
    g.creatureId = "D";
    g.x = 2.4f;
    g.y = 0.25f;
    g.z = -3.0f;
    g.yaw = -35.0f;
    g.pan = pan(-45.0f, 45.0f);
    g.elevation = elevation(-30.0f, 30.0f);
    g.cock = cock(-20.0f, 20.0f);
    return g;
}

// Relative pan angle (after yaw, before soft-clip) from one creature to another.
float relativePan(const GazeGeometry &from, const GazeGeometry &to) {
    return creatures::normalizeDegrees(bearingDegrees(from.x, from.z, to.x, to.z) - from.yaw);
}

float elevationTo(const GazeGeometry &from, const GazeGeometry &to) {
    return elevationDegrees(from.x, from.y, from.z, to.x, to.y, to.z);
}

} // namespace

// ---------------------------------------------------------------------------
// Bearing convention
// ---------------------------------------------------------------------------

TEST(GazeGeometryTest, BearingZeroPointsTowardPositiveZ) {
    EXPECT_NEAR(bearingDegrees(0.0f, 0.0f, 0.0f, 1.0f), 0.0f, 0.001f);
}

TEST(GazeGeometryTest, BearingNinetyPointsTowardPositiveX) {
    EXPECT_NEAR(bearingDegrees(0.0f, 0.0f, 1.0f, 0.0f), 90.0f, 0.001f);
    EXPECT_NEAR(bearingDegrees(0.0f, 0.0f, -1.0f, 0.0f), -90.0f, 0.001f);
}

TEST(GazeGeometryTest, BearingStraightBehindIsOneEighty) {
    EXPECT_NEAR(std::abs(bearingDegrees(0.0f, 0.0f, 0.0f, -1.0f)), 180.0f, 0.001f);
}

TEST(GazeGeometryTest, BearingOfCoincidentPointsIsZero) {
    EXPECT_NEAR(bearingDegrees(1.5f, -2.0f, 1.5f, -2.0f), 0.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// The plan's Layout A finding: creatures in a row need ~90 degrees to face
// each other. This is the whole reason the design leans on tilt.
// ---------------------------------------------------------------------------

TEST(GazeGeometryTest, SideBySideCreaturesNeedNearNinetyDegreesOfPan) {
    // Layout A yaws (+12 / +4 / -4 / -12) over the same positions.
    GazeGeometry a = creatureA();
    a.yaw = 12.0f;
    GazeGeometry b = creatureB();
    b.yaw = 4.0f;

    EXPECT_NEAR(relativePan(a, b), 92.0f, 0.5f);
    EXPECT_NEAR(relativePan(b, a), -80.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// Layout B relative pan angles — the plan's table, pre-soft-clip.
// ---------------------------------------------------------------------------

TEST(GazeGeometryTest, LayoutBRelativePanAnglesMatchThePlan) {
    const auto a = creatureA();
    const auto b = creatureB();
    const auto c = creatureC();
    const auto d = creatureD();

    EXPECT_NEAR(relativePan(a, b), 69.0f, 0.5f);
    EXPECT_NEAR(relativePan(a, c), 62.1f, 0.5f);
    EXPECT_NEAR(relativePan(a, d), 55.0f, 0.5f);

    EXPECT_NEAR(relativePan(b, a), -91.0f, 0.5f);
    EXPECT_NEAR(relativePan(b, c), 75.0f, 0.5f);
    EXPECT_NEAR(relativePan(b, d), 67.9f, 0.5f);

    EXPECT_NEAR(relativePan(c, a), -67.9f, 0.5f);
    EXPECT_NEAR(relativePan(c, b), -75.0f, 0.5f);
    EXPECT_NEAR(relativePan(c, d), 91.0f, 0.5f);

    EXPECT_NEAR(relativePan(d, a), -55.0f, 0.5f);
    EXPECT_NEAR(relativePan(d, b), -62.1f, 0.5f);
    EXPECT_NEAR(relativePan(d, c), -69.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// Layout B elevations — the plan's tilt table. These are the load-bearing
// numbers: tilt is what disambiguates *which* creature is being addressed.
// ---------------------------------------------------------------------------

TEST(GazeGeometryTest, LayoutBElevationsMatchThePlan) {
    const auto a = creatureA();
    const auto b = creatureB();
    const auto c = creatureC();
    const auto d = creatureD();

    EXPECT_NEAR(elevationTo(a, b), -13.6f, 0.3f);
    EXPECT_NEAR(elevationTo(a, c), -10.5f, 0.3f);
    EXPECT_NEAR(elevationTo(a, d), 1.8f, 0.3f);

    EXPECT_NEAR(elevationTo(b, a), 13.6f, 0.3f);
    EXPECT_NEAR(elevationTo(b, c), -7.1f, 0.3f);
    EXPECT_NEAR(elevationTo(b, d), 9.7f, 0.3f);

    EXPECT_NEAR(elevationTo(c, a), 10.5f, 0.3f);
    EXPECT_NEAR(elevationTo(c, b), 7.1f, 0.3f);
    EXPECT_NEAR(elevationTo(c, d), 24.5f, 0.3f);

    EXPECT_NEAR(elevationTo(d, a), -1.8f, 0.3f);
    EXPECT_NEAR(elevationTo(d, b), -9.7f, 0.3f);
    EXPECT_NEAR(elevationTo(d, c), -24.5f, 0.3f);
}

TEST(GazeGeometryTest, ElevationIsAntisymmetric) {
    const auto a = creatureA();
    const auto d = creatureD();
    EXPECT_NEAR(elevationTo(a, d), -elevationTo(d, a), 0.01f);
}

// ---------------------------------------------------------------------------
// Calibration: byte mapping, including the inverted-motor case.
// ---------------------------------------------------------------------------

TEST(GazeCalibrationTest, CentreOfSweepMapsToOneTwentyEight) {
    EXPECT_EQ(angleToByte(pan(-55.0f, 55.0f), 0.0f), 128);
    // Inverted motor: same physical centre, same byte. This is the 3.15.3
    // lesson — inversion must fall out of the calibration, not a separate flag.
    EXPECT_EQ(angleToByte(pan(60.0f, -60.0f), 0.0f), 128);
}

TEST(GazeCalibrationTest, EndsOfSweepMapToEnds) {
    EXPECT_EQ(angleToByte(pan(-55.0f, 55.0f), -55.0f), 0);
    EXPECT_EQ(angleToByte(pan(-55.0f, 55.0f), 55.0f), 255);
}

TEST(GazeCalibrationTest, InvertedMotorRunsBackwards) {
    const auto inverted = pan(60.0f, -60.0f);
    EXPECT_EQ(angleToByte(inverted, 60.0f), 0);
    EXPECT_EQ(angleToByte(inverted, -60.0f), 255);
    // Positive angle => low byte on an inverted axis.
    EXPECT_LT(angleToByte(inverted, 30.0f), 128);
}

TEST(GazeCalibrationTest, OutOfRangeAnglesClampRatherThanWrap) {
    const auto axis = pan(-55.0f, 55.0f);
    EXPECT_EQ(angleToByte(axis, 500.0f), 255);
    EXPECT_EQ(angleToByte(axis, -500.0f), 0);
}

TEST(GazeCalibrationTest, CentreAndRangeHandleAsymmetricSweeps) {
    const auto asymmetric = pan(-30.0f, 70.0f);
    EXPECT_NEAR(asymmetric.centre(), 20.0f, 0.001f);
    EXPECT_NEAR(asymmetric.range(), 50.0f, 0.001f);
    // Range is a magnitude regardless of axis direction.
    EXPECT_NEAR(pan(60.0f, -60.0f).range(), 60.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// Soft clip
// ---------------------------------------------------------------------------

TEST(GazeSoftClipTest, StaysInsideTheRange) {
    for (float angle = -200.0f; angle <= 200.0f; angle += 7.0f) {
        const float clipped = softClip(angle, 0.0f, 55.0f);
        EXPECT_LT(std::abs(clipped), 55.0f) << "angle " << angle;
    }
}

TEST(GazeSoftClipTest, IsMonotonic) {
    float previous = softClip(-200.0f, 0.0f, 55.0f);
    for (float angle = -199.0f; angle <= 200.0f; angle += 1.0f) {
        const float current = softClip(angle, 0.0f, 55.0f);
        EXPECT_GT(current, previous) << "angle " << angle;
        previous = current;
    }
}

TEST(GazeSoftClipTest, IsNearlyLinearNearTheCentre) {
    // Small angles should pass through close to untouched, so a creature
    // looking slightly off-centre isn't visibly compressed.
    EXPECT_NEAR(softClip(5.0f, 0.0f, 55.0f), 5.0f, 0.1f);
}

TEST(GazeSoftClipTest, KeepsFarApartTargetsDistinguishable) {
    // The plan's key claim about pan: saturated targets stay *slightly*
    // separated rather than collapsing to an identical value.
    const float toB = softClip(69.0f, 0.0f, 55.0f);
    const float toD = softClip(55.0f, 0.0f, 55.0f);
    EXPECT_GT(toB, toD);
    EXPECT_LT(toB - toD, 10.0f); // ...but only slightly. Hence tilt.
}

TEST(GazeSoftClipTest, DegenerateRangeReturnsCentre) { EXPECT_NEAR(softClip(40.0f, 12.0f, 0.0f), 12.0f, 0.001f); }

TEST(GazeSoftClipTest, RespectsAnAsymmetricCentre) {
    // With centre 20 and range 50, a huge angle approaches 70, not 50.
    EXPECT_NEAR(softClip(1000.0f, 20.0f, 50.0f), 70.0f, 0.5f);
}

// ---------------------------------------------------------------------------
// The plan's headline finding, asserted end to end: pan separates targets by
// only a few bytes, tilt separates them by dozens.
// ---------------------------------------------------------------------------

TEST(GazeGeometryTest, ElevationDisambiguatesTargetsFarBetterThanPan) {
    const auto c = creatureC();
    const auto a = creatureA();
    const auto b = creatureB();

    const auto panAt = [&](const GazeGeometry &target) {
        return angleToByte(*c.pan, softClip(relativePan(c, target), c.pan->centre(), c.pan->range()));
    };
    const auto elevationAt = [&](const GazeGeometry &target) {
        return angleToByte(*c.elevation, softClip(elevationTo(c, target), c.elevation->centre(), c.elevation->range()));
    };

    const int panSpread = std::abs(static_cast<int>(panAt(a)) - static_cast<int>(panAt(b)));
    const int elevationSpread = std::abs(static_cast<int>(elevationAt(a)) - static_cast<int>(elevationAt(b)));

    EXPECT_LT(panSpread, 10) << "pan should be nearly degenerate between neighbours";
    EXPECT_GT(elevationSpread, 10) << "elevation is what carries target identity";
    EXPECT_GT(elevationSpread, panSpread);
}

// ---------------------------------------------------------------------------
// Speaker timeline
// ---------------------------------------------------------------------------

TEST(SpeakerTimelineTest, FindsOneSpanPerContiguousRun) {
    const std::vector<uint8_t> mouthA{0, 0, 3, 3, 3, 0, 0, 0};
    const std::vector<uint8_t> mouthB{0, 0, 0, 0, 0, 0, 4, 4};
    const std::vector<std::string> ids{"A", "B"};
    const std::vector<std::span<const uint8_t>> mouths{mouthA, mouthB};

    const auto spans = buildSpeakerTimeline(ids, mouths, 8, 0);
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].creatureId, "A");
    EXPECT_EQ(spans[0].startFrame, 2u);
    EXPECT_EQ(spans[0].endFrame, 5u);
    EXPECT_EQ(spans[1].creatureId, "B");
    EXPECT_EQ(spans[1].startFrame, 6u);
}

TEST(SpeakerTimelineTest, MergesShortGapsWithinOneTurn) {
    // Between-word silence must not shred a turn into many spans, or every
    // listener would re-aim dozens of times per sentence.
    const std::vector<uint8_t> mouthA{3, 3, 0, 0, 3, 3, 0, 0};
    const std::vector<std::string> ids{"A"};
    const std::vector<std::span<const uint8_t>> mouths{mouthA};

    const auto merged = buildSpeakerTimeline(ids, mouths, 8, 5);
    ASSERT_EQ(merged.size(), 1u);
    EXPECT_EQ(merged[0].startFrame, 0u);
    EXPECT_EQ(merged[0].endFrame, 6u);

    const auto split = buildSpeakerTimeline(ids, mouths, 8, 0);
    EXPECT_EQ(split.size(), 2u);
}

TEST(SpeakerTimelineTest, SpansAreOrderedByStart) {
    const std::vector<uint8_t> mouthA{0, 0, 0, 0, 5, 5};
    const std::vector<uint8_t> mouthB{5, 5, 0, 0, 0, 0};
    const std::vector<std::string> ids{"A", "B"};
    const std::vector<std::span<const uint8_t>> mouths{mouthA, mouthB};

    const auto spans = buildSpeakerTimeline(ids, mouths, 6, 0);
    ASSERT_EQ(spans.size(), 2u);
    EXPECT_EQ(spans[0].creatureId, "B");
    EXPECT_EQ(spans[1].creatureId, "A");
}

TEST(SpeakerTimelineTest, MismatchedInputSizesProduceNothing) {
    const std::vector<uint8_t> mouthA{5, 5};
    const std::vector<std::string> ids{"A", "B"};
    const std::vector<std::span<const uint8_t>> mouths{mouthA};
    EXPECT_TRUE(buildSpeakerTimeline(ids, mouths, 2, 0).empty());
}

// ---------------------------------------------------------------------------
// Track construction
// ---------------------------------------------------------------------------

namespace {

std::vector<SpeakerSpan> twoTurnScene() { return {SpeakerSpan{0, 100, "A"}, SpeakerSpan{140, 260, "B"}}; }

} // namespace

TEST(GazeTrackTest, CreatureWithNoAxesProducesNothing) {
    GazeGeometry self = creatureA();
    self.pan.reset();
    self.elevation.reset();
    self.cock.reset();
    std::mt19937 rng(1234);
    const auto track = buildGazeTrack(self, {creatureB()}, twoTurnScene(), 300, 20, rng);
    EXPECT_TRUE(track.empty());
}

TEST(GazeTrackTest, PanOnlyCreatureGetsPanBytesAndNothingElse) {
    GazeGeometry self = creatureA();
    self.elevation.reset();
    self.cock.reset();
    std::mt19937 rng(1234);
    const auto track = buildGazeTrack(self, {creatureB()}, twoTurnScene(), 300, 20, rng);
    EXPECT_EQ(track.panBytes.size(), 300u);
    EXPECT_TRUE(track.elevationBytes.empty());
    EXPECT_TRUE(track.cockBytes.empty());
    // The slot rides along so the caller knows where to write it.
    EXPECT_EQ(track.panSlot, 2u);
}

TEST(GazeTrackTest, SameSeedReproducesIdenticalBytes) {
    // This is what makes re-rendering against a moved stage safe: with the
    // seed pinned, nothing but the geometry can change.
    const auto self = creatureA();
    const std::vector<GazeGeometry> others{creatureB(), creatureC()};

    std::mt19937 rngA(99);
    std::mt19937 rngB(99);
    const auto first = buildGazeTrack(self, others, twoTurnScene(), 300, 20, rngA);
    const auto second = buildGazeTrack(self, others, twoTurnScene(), 300, 20, rngB);

    EXPECT_EQ(first.panBytes, second.panBytes);
    EXPECT_EQ(first.elevationBytes, second.elevationBytes);
    EXPECT_EQ(first.cockBytes, second.cockBytes);
}

TEST(GazeTrackTest, DifferentSeedsProduceDifferentTiming) {
    const auto self = creatureA();
    const std::vector<GazeGeometry> others{creatureB(), creatureC()};

    std::mt19937 rngA(1);
    std::mt19937 rngB(2);
    const auto first = buildGazeTrack(self, others, twoTurnScene(), 300, 20, rngA);
    const auto second = buildGazeTrack(self, others, twoTurnScene(), 300, 20, rngB);

    EXPECT_NE(first.panBytes, second.panBytes);
}

TEST(GazeTrackTest, HeadActuallyMovesWhenTheSpeakerChanges) {
    const auto self = creatureA();
    std::mt19937 rng(7);
    // A speaks first (so A addresses the listener), then B takes over and A
    // should swing round to watch B. ignoreTurnChance is pinned off — with it
    // live, the single transition in this scene can legitimately be skipped,
    // which would make the assertion seed-dependent.
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    const auto track = buildGazeTrack(self, {creatureB()}, twoTurnScene(), 400, 20, rng, options);
    ASSERT_EQ(track.panBytes.size(), 400u);

    const auto atStart = track.panBytes[10];
    const auto atEnd = track.panBytes[399];
    EXPECT_GT(std::abs(static_cast<int>(atEnd) - static_cast<int>(atStart)), 40)
        << "expected a large, visible pan sweep when the floor changed";
}

TEST(GazeTrackTest, ReactionIsDelayedRatherThanInstant) {
    const auto self = creatureA();
    std::mt19937 rng(7);
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    const auto track = buildGazeTrack(self, {creatureB()}, twoTurnScene(), 400, 20, rng, options);

    // The turn changes at frame 140. With a minimum 150 ms reaction at
    // 20 ms/frame, nothing should have moved by frame 145.
    EXPECT_EQ(track.panBytes[141], track.panBytes[140]);
    EXPECT_EQ(track.panBytes[145], track.panBytes[140]);
}

TEST(GazeTrackTest, CreaturesDoNotAllMoveInUnison) {
    // Each creature draws its own reaction delay, so their sweeps must not be
    // frame-aligned. This is the behavior the whole jitter model exists for.
    const std::vector<GazeGeometry> everyone{creatureA(), creatureB(), creatureC(), creatureD()};
    const std::vector<SpeakerSpan> scene{SpeakerSpan{0, 60, "A"}, SpeakerSpan{100, 300, "D"}};

    GazeOptions options;
    options.ignoreTurnChance = 0.0f; // isolate timing spread from the skip draw

    std::vector<std::size_t> firstMovementFrames;
    for (std::size_t i = 0; i < everyone.size(); ++i) {
        std::mt19937 rng(static_cast<uint32_t>(500 + i));
        const auto track = buildGazeTrack(everyone[i], everyone, scene, 400, 20, rng, options);
        if (track.panBytes.empty()) {
            continue;
        }
        // Find the first frame after the turn change where the byte moves by
        // more than the micro-drift could account for.
        const auto reference = track.panBytes[100];
        std::size_t moved = 0;
        for (std::size_t f = 101; f < track.panBytes.size(); ++f) {
            if (std::abs(static_cast<int>(track.panBytes[f]) - static_cast<int>(reference)) > 12) {
                moved = f;
                break;
            }
        }
        if (moved != 0) {
            firstMovementFrames.push_back(moved);
        }
    }

    ASSERT_GE(firstMovementFrames.size(), 3u);
    const auto minFrame = *std::min_element(firstMovementFrames.begin(), firstMovementFrames.end());
    const auto maxFrame = *std::max_element(firstMovementFrames.begin(), firstMovementFrames.end());
    EXPECT_GT(maxFrame - minFrame, 3u) << "creatures started moving too close together";
}

TEST(GazeTrackTest, HoldingATargetStillDrifts) {
    // Nobody is ever perfectly frozen — a completely static servo value reads
    // as "powered off" rather than "watching".
    const auto self = creatureA();
    std::mt19937 rng(11);
    const auto track = buildGazeTrack(self, {creatureB()}, {SpeakerSpan{0, 400, "B"}}, 400, 20, rng);
    ASSERT_FALSE(track.panBytes.empty());

    // Well after the sweep has settled, values should still be varying.
    bool sawChange = false;
    for (std::size_t f = 301; f < 400; ++f) {
        if (track.panBytes[f] != track.panBytes[300]) {
            sawChange = true;
            break;
        }
    }
    EXPECT_TRUE(sawChange);
}

TEST(GazeTrackTest, UnknownSpeakerLeavesTheHeadWhereItWas) {
    // A speaker who isn't placed on this stage shouldn't cause a lurch.
    const auto self = creatureA();
    std::mt19937 rng(3);
    const std::vector<SpeakerSpan> scene{SpeakerSpan{0, 200, "somebody-not-on-stage"}};
    const auto track = buildGazeTrack(self, {creatureB()}, scene, 200, 20, rng);
    ASSERT_FALSE(track.panBytes.empty());

    for (std::size_t f = 1; f < track.panBytes.size(); ++f) {
        EXPECT_LT(std::abs(static_cast<int>(track.panBytes[f]) - static_cast<int>(track.panBytes[0])), 12);
    }
}

TEST(GazeTrackTest, ZeroFramesProducesNothing) {
    const auto self = creatureA();
    std::mt19937 rng(1);
    EXPECT_TRUE(buildGazeTrack(self, {creatureB()}, twoTurnScene(), 0, 20, rng).empty());
}

TEST(GazeTrackTest, ZeroMsPerFrameProducesNothing) {
    const auto self = creatureA();
    std::mt19937 rng(1);
    EXPECT_TRUE(buildGazeTrack(self, {creatureB()}, twoTurnScene(), 100, 0, rng).empty());
}

TEST(GazeTrackTest, OvershootNeverWrapsAround) {
    // Overshoot and drift both push past the target angle. If either escaped
    // the clamp in angleToByte, the cast to uint8_t would wrap and a servo
    // would slam to the opposite rail for a frame. A wrap shows up as an
    // impossibly large single-frame jump, so that's what we look for.
    const auto self = creatureA();
    std::mt19937 rng(42);
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    // Deliberately absurd overshoot + drift, far beyond the neck's range.
    options.minOvershootDegrees = 200.0f;
    options.maxOvershootDegrees = 400.0f;
    options.panDriftDegrees = 120.0f;
    const auto track = buildGazeTrack(self, {creatureB(), creatureC(), creatureD()},
                                      {SpeakerSpan{0, 50, "A"}, SpeakerSpan{60, 120, "D"}, SpeakerSpan{130, 200, "B"}},
                                      200, 20, rng, options);
    ASSERT_EQ(track.panBytes.size(), 200u);
    for (std::size_t f = 1; f < track.panBytes.size(); ++f) {
        const int jump = std::abs(static_cast<int>(track.panBytes[f]) - static_cast<int>(track.panBytes[f - 1]));
        EXPECT_LT(jump, 200) << "suspicious jump at frame " << f << " — likely a wrap rather than a sweep";
    }
}

TEST(GazeTrackTest, AlwaysIgnoringTurnsLeavesTheHeadPut) {
    // The other side of the skip draw: a creature that ignores every turn
    // change should never re-aim, only drift.
    const auto self = creatureA();
    std::mt19937 rng(7);
    GazeOptions options;
    options.ignoreTurnChance = 1.0f;
    const auto track = buildGazeTrack(self, {creatureB()}, twoTurnScene(), 400, 20, rng, options);
    ASSERT_EQ(track.panBytes.size(), 400u);

    for (std::size_t f = 0; f < track.panBytes.size(); ++f) {
        EXPECT_LT(std::abs(static_cast<int>(track.panBytes[f]) - static_cast<int>(track.panBytes[0])), 12)
            << "frame " << f;
    }
}

// ---------------------------------------------------------------------------
// normalizeDegrees — shared with the stage parser, and easy to get wrong at
// the wrap points.
// ---------------------------------------------------------------------------

TEST(NormalizeDegreesTest, LeavesInRangeAnglesAlone) {
    EXPECT_NEAR(creatures::normalizeDegrees(0.0f), 0.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(90.0f), 90.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(-90.0f), -90.0f, 0.001f);
}

TEST(NormalizeDegreesTest, WrapsBeyondHalfTurn) {
    EXPECT_NEAR(creatures::normalizeDegrees(190.0f), -170.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(-190.0f), 170.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(360.0f), 0.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(720.0f + 45.0f), 45.0f, 0.001f);
}

TEST(NormalizeDegreesTest, PicksPositiveOneEightyConsistently) {
    EXPECT_NEAR(creatures::normalizeDegrees(180.0f), 180.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(-180.0f), 180.0f, 0.001f);
}

TEST(NormalizeDegreesTest, HandlesNonFiniteInput) {
    EXPECT_NEAR(creatures::normalizeDegrees(std::numeric_limits<float>::quiet_NaN()), 0.0f, 0.001f);
    EXPECT_NEAR(creatures::normalizeDegrees(std::numeric_limits<float>::infinity()), 0.0f, 0.001f);
}

// ---------------------------------------------------------------------------
// resolveGazeGeometry — the name-to-slot lookup. This is the whole reason the
// config names an input instead of numbering a slot, so it needs real coverage
// against BOTH creature families' layouts.
// ---------------------------------------------------------------------------

namespace {

/// Beaky / Crow input layout.
creatures::Creature creatureWithBeakyLayout() {
    creatures::Creature c;
    c.id = "beaky";
    c.name = "Beaky";
    c.inputs = {{"head_tilt", 0, 1, 0}, {"head_height", 1, 1, 1}, {"neck_rotate", 2, 1, 2}, {"stand_rotate", 3, 1, 3},
                {"body_lean", 4, 1, 4}, {"beak", 5, 1, 5},        {"chest", 6, 1, 6}};
    return c;
}

/// Mango / Kenny lay the same named inputs out at different slots — the exact
/// situation a hand-copied slot number would get wrong.
creatures::Creature creatureWithMangoLayout() {
    creatures::Creature c;
    c.id = "mango";
    c.name = "Mango";
    c.inputs = {{"stand_rotate", 0, 1, 0}, {"beak", 2, 1, 2},      {"head_tilt", 3, 1, 3}, {"head_height", 4, 1, 4},
                {"neck_rotate", 5, 1, 5},  {"body_lean", 6, 1, 6}, {"chest", 7, 1, 7}};
    return c;
}

creatures::GazeConfig standardGazeConfig() {
    creatures::GazeConfig gaze;
    gaze.pan = creatures::GazeAxis{"neck_rotate", -55.0f, 55.0f, 0.4f};
    gaze.elevation = creatures::GazeAxis{"head_height", -25.0f, 25.0f, 0.4f};
    gaze.cock = creatures::GazeAxis{"head_tilt", -20.0f, 20.0f, 0.4f};
    return gaze;
}

creatures::StagePlacement placementAt(float x, float y, float z, float yaw) {
    creatures::StagePlacement p;
    p.creature_id = "whoever";
    p.x = x;
    p.y = y;
    p.z = z;
    p.yaw = yaw;
    return p;
}

} // namespace

TEST(ResolveGazeGeometryTest, ResolvesSlotsFromInputNames) {
    auto creature = creatureWithBeakyLayout();
    creature.gaze = standardGazeConfig();

    const auto geometry = resolveGazeGeometry(creature, placementAt(-2.4f, 0.1f, -3.0f, 35.0f));

    ASSERT_TRUE(geometry.pan.has_value());
    ASSERT_TRUE(geometry.elevation.has_value());
    ASSERT_TRUE(geometry.cock.has_value());
    EXPECT_EQ(geometry.pan->slot, 2u);       // neck_rotate
    EXPECT_EQ(geometry.elevation->slot, 1u); // head_height
    EXPECT_EQ(geometry.cock->slot, 0u);      // head_tilt
}

TEST(ResolveGazeGeometryTest, SameConfigResolvesDifferentlyPerCreature) {
    // The load-bearing case: identical `gaze` config, different input layouts,
    // and the slots must follow the names rather than being fixed.
    auto beaky = creatureWithBeakyLayout();
    beaky.gaze = standardGazeConfig();
    auto mango = creatureWithMangoLayout();
    mango.gaze = standardGazeConfig();

    const auto beakyGeometry = resolveGazeGeometry(beaky, placementAt(0, 0, -3, 0));
    const auto mangoGeometry = resolveGazeGeometry(mango, placementAt(0, 0, -3, 0));

    EXPECT_EQ(beakyGeometry.pan->slot, 2u);
    EXPECT_EQ(mangoGeometry.pan->slot, 5u);
    EXPECT_EQ(beakyGeometry.elevation->slot, 1u);
    EXPECT_EQ(mangoGeometry.elevation->slot, 4u);
    EXPECT_EQ(beakyGeometry.cock->slot, 0u);
    EXPECT_EQ(mangoGeometry.cock->slot, 3u);
}

TEST(ResolveGazeGeometryTest, CarriesPlacementThrough) {
    auto creature = creatureWithBeakyLayout();
    creature.gaze = standardGazeConfig();
    const auto geometry = resolveGazeGeometry(creature, placementAt(-2.4f, 0.1f, -3.0f, 35.0f));

    EXPECT_EQ(geometry.creatureId, "beaky");
    EXPECT_NEAR(geometry.x, -2.4f, 0.001f);
    EXPECT_NEAR(geometry.y, 0.1f, 0.001f);
    EXPECT_NEAR(geometry.z, -3.0f, 0.001f);
    EXPECT_NEAR(geometry.yaw, 35.0f, 0.001f);
}

TEST(ResolveGazeGeometryTest, CreatureWithNoGazeConfigGetsNoAxes) {
    const auto creature = creatureWithBeakyLayout(); // gaze left unset
    const auto geometry = resolveGazeGeometry(creature, placementAt(0, 0, -3, 0));

    EXPECT_FALSE(geometry.pan.has_value());
    EXPECT_FALSE(geometry.elevation.has_value());
    EXPECT_FALSE(geometry.cock.has_value());
}

TEST(ResolveGazeGeometryTest, AxisNamingAMissingInputIsDroppedNotGuessed) {
    // Rejected at upload time, so this only happens if a stored config drifted.
    // Dropping the axis is right; inventing a slot would drive a random servo.
    auto creature = creatureWithBeakyLayout();
    creatures::GazeConfig gaze;
    gaze.pan = creatures::GazeAxis{"neck_rotate", -55.0f, 55.0f, 0.4f};
    gaze.elevation = creatures::GazeAxis{"wing_flap", -25.0f, 25.0f, 0.4f}; // no such input
    creature.gaze = gaze;

    const auto geometry = resolveGazeGeometry(creature, placementAt(0, 0, -3, 0));
    EXPECT_TRUE(geometry.pan.has_value());
    EXPECT_FALSE(geometry.elevation.has_value());
}

TEST(ResolveGazeGeometryTest, CalibrationDegreesSurviveResolution) {
    auto creature = creatureWithBeakyLayout();
    creatures::GazeConfig gaze;
    // Reversed axis, expressed by swapping min and max.
    gaze.pan = creatures::GazeAxis{"neck_rotate", 60.0f, -60.0f, 0.4f};
    creature.gaze = gaze;

    const auto geometry = resolveGazeGeometry(creature, placementAt(0, 0, -3, 0));
    ASSERT_TRUE(geometry.pan.has_value());
    EXPECT_NEAR(geometry.pan->degreesAtMin, 60.0f, 0.001f);
    EXPECT_NEAR(geometry.pan->degreesAtMax, -60.0f, 0.001f);
    EXPECT_NEAR(geometry.pan->centre(), 0.0f, 0.001f);
    EXPECT_NEAR(geometry.pan->range(), 60.0f, 0.001f);
    // Centre still maps to 128 despite the reversal.
    EXPECT_EQ(angleToByte(*geometry.pan, 0.0f), 128);
}

// ---------------------------------------------------------------------------
// Head cock
// ---------------------------------------------------------------------------

TEST(GazeCockTest, SpeakingCreatureHoldsItsHeadLevel) {
    // A creature that's talking isn't listening, so it shouldn't be cocking
    // its head at anyone.
    const auto self = creatureA();
    std::mt19937 rng(5);
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    const std::vector<SpeakerSpan> allSelf{SpeakerSpan{0, 400, "A"}};
    const auto track = buildGazeTrack(self, {creatureB()}, allSelf, 400, 20, rng, options);

    ASSERT_EQ(track.cockBytes.size(), 400u);
    for (std::size_t f = 0; f < track.cockBytes.size(); ++f) {
        EXPECT_NEAR(static_cast<int>(track.cockBytes[f]), 128, 2) << "frame " << f;
    }
}

TEST(GazeCockTest, ListeningCreatureCanCockItsHead) {
    const auto self = creatureA();
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f; // pin it on so the assertion isn't seed-dependent

    std::mt19937 rng(5);
    const std::vector<SpeakerSpan> otherSpeaks{SpeakerSpan{0, 400, "B"}};
    const auto track = buildGazeTrack(self, {creatureB()}, otherSpeaks, 400, 20, rng, options);

    ASSERT_EQ(track.cockBytes.size(), 400u);
    bool cocked = false;
    for (const auto value : track.cockBytes) {
        if (std::abs(static_cast<int>(value) - 128) > 5) {
            cocked = true;
            break;
        }
    }
    EXPECT_TRUE(cocked) << "a listening creature should adopt a sideways head tilt";
}

TEST(GazeCockTest, ListeningAmountZeroDisablesTheCock) {
    GazeGeometry self = creatureA();
    self.cock->listeningAmount = 0.0f;
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;

    std::mt19937 rng(5);
    const auto track = buildGazeTrack(self, {creatureB()}, {SpeakerSpan{0, 400, "B"}}, 400, 20, rng, options);

    ASSERT_EQ(track.cockBytes.size(), 400u);
    for (const auto value : track.cockBytes) {
        EXPECT_NEAR(static_cast<int>(value), 128, 2);
    }
}

TEST(GazeCockTest, CockDoesNotDrift) {
    // Pan and elevation wander slowly so nobody looks powered-off, but a
    // wandering head tilt reads as a fault rather than as life.
    GazeGeometry self = creatureA();
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;

    std::mt19937 rng(5);
    const auto track = buildGazeTrack(self, {creatureB()}, {SpeakerSpan{0, 400, "B"}}, 400, 20, rng, options);

    ASSERT_EQ(track.cockBytes.size(), 400u);
    // Well after the lean has settled, it should be perfectly still.
    for (std::size_t f = 301; f < 400; ++f) {
        EXPECT_EQ(track.cockBytes[f], track.cockBytes[300]) << "frame " << f;
    }
}

TEST(GazeCockTest, ACockPersistsAcrossTurnsRatherThanRedrawing) {
    // The dog property: once the head is cocked it stays there through the
    // conversation. Re-drawing the angle on every turn would read as a twitch.
    GazeGeometry self = creatureA();
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;
    options.cockPersistence = 1.0f; // pin it so the assertion isn't seed-dependent

    // A rapid four-way exchange — exactly the case where re-cocking every turn
    // would look fidgety.
    const std::vector<SpeakerSpan> rapidExchange{SpeakerSpan{0, 40, "B"}, SpeakerSpan{50, 90, "C"},
                                                 SpeakerSpan{100, 140, "D"}, SpeakerSpan{150, 190, "B"},
                                                 SpeakerSpan{200, 240, "C"}};

    std::mt19937 rng(21);
    const auto track =
        buildGazeTrack(self, {creatureB(), creatureC(), creatureD()}, rapidExchange, 400, 20, rng, options);
    ASSERT_EQ(track.cockBytes.size(), 400u);

    // Count how many distinct settled values the cock takes. With persistence
    // pinned on it should adopt one lean and keep it.
    std::set<int> settledValues;
    for (std::size_t f = 300; f < 400; ++f) {
        settledValues.insert(track.cockBytes[f]);
    }
    EXPECT_EQ(settledValues.size(), 1u) << "the cock should be perfectly still once held";

    // And it should be an actual lean, not level.
    EXPECT_GT(std::abs(static_cast<int>(track.cockBytes[399]) - 128), 5);
}

TEST(GazeCockTest, ZeroPersistenceLetsTheCockChange) {
    // The other side of the same knob, so the test above is really testing
    // persistence rather than something incidental.
    GazeGeometry self = creatureA();
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;
    options.cockPersistence = 0.0f;
    options.preferredSideBias = 0.5f; // let it use both sides

    const std::vector<SpeakerSpan> rapidExchange{SpeakerSpan{0, 40, "B"},    SpeakerSpan{50, 90, "C"},
                                                 SpeakerSpan{100, 140, "D"}, SpeakerSpan{150, 190, "B"},
                                                 SpeakerSpan{200, 240, "C"}, SpeakerSpan{250, 290, "D"}};

    std::mt19937 rng(21);
    const auto track =
        buildGazeTrack(self, {creatureB(), creatureC(), creatureD()}, rapidExchange, 400, 20, rng, options);

    std::set<int> values(track.cockBytes.begin(), track.cockBytes.end());
    EXPECT_GT(values.size(), 2u) << "with no persistence the lean should change between turns";
}

TEST(GazeCockTest, EachCreatureFavoursOneSide) {
    // Dogs favour a side; a bird that alternates at random looks confused.
    // With the bias pinned to 1.0 every cock must land on the same side.
    GazeGeometry self = creatureA();
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;
    options.cockPersistence = 0.0f; // force a fresh draw every turn
    options.preferredSideBias = 1.0f;

    const std::vector<SpeakerSpan> exchange{SpeakerSpan{0, 40, "B"}, SpeakerSpan{50, 90, "C"},
                                            SpeakerSpan{100, 140, "D"}, SpeakerSpan{150, 190, "B"}};

    std::mt19937 rng(33);
    const auto track = buildGazeTrack(self, {creatureB(), creatureC(), creatureD()}, exchange, 300, 20, rng, options);
    ASSERT_EQ(track.cockBytes.size(), 300u);

    // Every cocked frame should be on the same side of centre.
    bool sawAbove = false;
    bool sawBelow = false;
    for (const auto value : track.cockBytes) {
        const int delta = static_cast<int>(value) - 128;
        if (delta > 5) {
            sawAbove = true;
        }
        if (delta < -5) {
            sawBelow = true;
        }
    }
    EXPECT_TRUE(sawAbove != sawBelow) << "cocks landed on both sides despite a pinned side preference";
}

TEST(GazeCockTest, DifferentCreaturesCanFavourDifferentSides) {
    // The preferred side is drawn per creature, so across a cast they differ.
    GazeOptions options;
    options.ignoreTurnChance = 0.0f;
    options.chanceOfCock = 1.0f;
    options.preferredSideBias = 1.0f;
    const std::vector<SpeakerSpan> scene{SpeakerSpan{0, 300, "B"}};

    std::set<int> sides;
    for (uint32_t seed = 0; seed < 12; ++seed) {
        std::mt19937 rng(seed);
        const auto track = buildGazeTrack(creatureA(), {creatureB()}, scene, 300, 20, rng, options);
        if (track.cockBytes.empty()) {
            continue;
        }
        const int delta = static_cast<int>(track.cockBytes[299]) - 128;
        if (std::abs(delta) > 5) {
            sides.insert(delta > 0 ? 1 : -1);
        }
    }
    EXPECT_EQ(sides.size(), 2u) << "across seeds, creatures should favour both sides";
}
