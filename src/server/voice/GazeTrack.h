#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "model/Creature.h"
#include "model/Stage.h"

// Stage-aware head aiming (issue #119).
//
// Everything here is pure: no database, no network, no clock. Given where the
// creatures are, which way they face, how their neck servos are calibrated,
// and who is speaking when, it produces per-frame servo bytes for the head
// axes. That makes the whole behavior unit-testable against fixed numbers,
// which matters because the geometry is easy to get subtly wrong (a sign flip
// on yaw sends every head the wrong way and still "looks plausible" in code).
//
// ---------------------------------------------------------------------------
// What the geometry is actually worth
// ---------------------------------------------------------------------------
// Modeling four creatures in a row showed that PAN and ELEVATION carry
// different information, and it drives the design:
//
//   * Creatures standing side by side need 78-92 degrees of yaw to face each
//     other, which exceeds any real neck. So every creature-to-creature pan
//     target saturates at roughly the same rail: aiming at one neighbour vs
//     another differs by only 3-11 servo bytes, under the mechanical slop.
//     What pan DOES convey, unmistakably, is which *side* a creature turned
//     toward — around 100 bytes of travel.
//
//   * Elevation does not saturate, because the perches sit at different
//     heights. It separates targets by 66-96 bytes. So elevation is what
//     tells the audience *which* creature is being addressed.
//
// Pan says which side; elevation says which one. Both are worth driving, and
// neither substitutes for the other.
//
// A third axis, the head COCK, aims at nothing — it's the quizzical sideways
// tilt a listening bird does. Purely expressive, but it's a strong "this one
// is paying attention" signal that costs almost nothing to add.
//
// ---------------------------------------------------------------------------
// Which physical axis is which
// ---------------------------------------------------------------------------
// The birds have a DIFFERENTIAL neck: the controller's DifferentialHead turns
// head_height + head_tilt into neck_left/neck_right positions. Both servos
// moving together raises the head; moving apart cocks it sideways. So:
//
//   pan       <- neck_rotate   (a plain servo, turns left/right)
//   elevation <- head_height   (NOT head_tilt — that's the cock)
//   cock      <- head_tilt
//
// The server works in INPUT space throughout; the controller owns the
// differential maths and the servo-level inversion flags.
//
// The second consequence is that precise target selection buys far less than
// believable timing. Hence the jitter model below, which is where most of the
// naturalism actually comes from.
namespace creatures::voice {

/// One gaze axis with its input slot already resolved. The name-to-slot
/// lookup happens once, in resolveGazeGeometry, so the per-frame maths never
/// touches strings.
struct ResolvedGazeAxis {
    std::size_t slot{0};
    float degreesAtMin{0.0f};
    float degreesAtMax{0.0f};
    float listeningAmount{0.4f}; // cock axis only

    [[nodiscard]] float centre() const { return (degreesAtMin + degreesAtMax) * 0.5f; }
    [[nodiscard]] float range() const { return std::abs(degreesAtMax - degreesAtMin) * 0.5f; }
};

/// Everything the gaze math needs about one creature: where it is, which way
/// it faces, and how its inputs map bytes to angles. Assembled by the caller
/// from a Stage placement plus the creature's config.
struct GazeGeometry {
    std::string creatureId;

    // Stage frame — metres, listener at the origin facing -Z. See
    // model/Stage.h for the full definition.
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};

    // Which way this creature faces, degrees, copied verbatim from the stage
    // placement: 0 = facing +Z, +90 = facing -X. Same rotational sense as
    // bearingDegrees() above, so the two subtract directly.
    float yaw{0.0f};

    // Absent axis = that degree of freedom isn't driven. A creature with none
    // of them is left entirely alone.
    std::optional<ResolvedGazeAxis> pan;       // left/right, normally neck_rotate
    std::optional<ResolvedGazeAxis> elevation; // up/down, normally head_height
    // Sideways tilt, normally head_tilt. Note the track builder only WRITES
    // this while the creature is listening — a speaking creature's tilt is left
    // to its body loop (issue #144). The stream is still computed for every
    // frame so the rng consumption stays fixed.
    std::optional<ResolvedGazeAxis> cock;
};

/// Resolve a creature's `gaze` config into slot-indexed axes by looking each
/// axis's named input up in `creature.inputs`. Returns the geometry with only
/// the axes that resolved — an axis naming a missing input is dropped rather
/// than guessed at. (The creature parser rejects that case on upload, so it
/// should only happen if a config changed underneath a stored document.)
[[nodiscard]] GazeGeometry resolveGazeGeometry(const creatures::Creature &creature,
                                               const creatures::StagePlacement &placement);

/// One creature holding the floor over a half-open frame range.
struct SpeakerSpan {
    std::size_t startFrame{0};
    std::size_t endFrame{0}; // exclusive
    std::string creatureId;
};

/// Per-frame servo bytes for one creature's head. An empty vector means that
/// axis isn't driven and the caller must leave those bytes untouched — which
/// is different from "drive it to its centre".
struct GazeTrack {
    std::vector<uint8_t> panBytes;
    std::vector<uint8_t> elevationBytes;
    std::vector<uint8_t> cockBytes;

    // Slots the caller should write each stream into, copied from the
    // resolved geometry so the caller doesn't have to carry it separately.
    std::size_t panSlot{0};
    std::size_t elevationSlot{0};
    std::size_t cockSlot{0};

    [[nodiscard]] bool empty() const { return panBytes.empty() && elevationBytes.empty() && cockBytes.empty(); }
};

/// Tunables for the naturalism model. Defaults are the values modeled in
/// docs/idle-and-stage-gaze-plan.md; they're grouped here so they can be
/// adjusted against real hardware without hunting through the implementation.
struct GazeOptions {
    // How long after a turn changes before this creature reacts. Drawn per
    // creature per transition — the spread across creatures is what stops four
    // heads snapping on the same cue, which is the single creepiest failure.
    uint32_t minReactionMs{150};
    uint32_t maxReactionMs{550};

    // How long the pan sweep takes. Elevation runs slightly faster (a real
    // head settles its height before it finishes swinging).
    uint32_t minPanTravelMs{250};
    uint32_t maxPanTravelMs{450};
    float elevationTravelScaleMin{0.75f};
    float elevationTravelScaleMax{0.95f};

    // Overshoot past the target, then settle back. Elevation takes half.
    float minOvershootDegrees{2.0f};
    float maxOvershootDegrees{6.0f};
    uint32_t settleMs{150};

    // Slow wander while holding a target, so nobody is ever perfectly still.
    float panDriftDegrees{1.5f};
    float elevationDriftDegrees{1.0f};
    uint32_t minDriftPeriodMs{4000};
    uint32_t maxDriftPeriodMs{9000};

    // ---- Head-cock ------------------------------------------------------
    // The dog thing: a listening creature drops one ear toward its shoulder
    // and holds it. A speaking one is level — you don't cock your head at
    // someone while you're the one talking.
    //
    // Two properties matter for this reading as curiosity rather than as a
    // twitch:
    //
    //   * It PERSISTS. A dog that cocks its head holds it there through the
    //     rest of the conversation; it doesn't re-cock on every sentence.
    //     cockPersistence is the chance of keeping the current lean when the
    //     floor changes, and it's deliberately high.
    //   * It has a SIDE. Dogs favour one side (usually tied to their dominant
    //     ear). Each creature draws a preferred side once per scene and
    //     mostly returns to it, which reads as personality rather than noise.
    float chanceOfCock{0.55f};     // chance of adopting a cock from level
    float cockPersistence{0.75f};  // chance of holding an existing cock
    float preferredSideBias{0.8f}; // chance of cocking to this creature's favoured side

    // ---- Settling home --------------------------------------------------
    // Once nobody holds the floor, heads ease back to a neutral head-forward
    // pose instead of staying locked on their last target. The hold is
    // deliberately longer than an ordinary pause between sentences — a breath
    // mid-conversation must not send four heads home and straight back.
    uint32_t neutralReturnHoldMs{1500};
    // How much slower settling home is than a head turn. Relaxing, not
    // reacting: it also skips the overshoot and the reaction delay entirely.
    //
    // This directly sets how early the wind-down starts, and therefore how much
    // of the scene the creatures spend actually looking at each other. Keep it
    // modest — 2.5 was tried on hardware and read as a long, listless drift.
    float neutralReturnTravelScale{1.4f};

    // Probability that a listener simply doesn't react to a given turn change
    // and keeps watching whoever it was already watching. Four creatures all
    // reacting to every single turn reads as mechanical.
    float ignoreTurnChance{0.15f};
};

// ---------------------------------------------------------------------------
// Geometry primitives — exposed so tests can pin them directly.
// ---------------------------------------------------------------------------

/// Bearing from one point to another in the stage's XZ plane, in degrees:
/// 0 = toward +Z, +90 = toward -X. Returns 0 for coincident points (a creature
/// can't meaningfully look at itself).
///
/// The +90-is-toward-MINUS-X part is load-bearing (issue #144). It puts this in
/// the same rotational sense as a StagePlacement's `yaw`, so the two subtract
/// directly, and it makes a positive relative angle mean "turn to your right" —
/// which is how a `degrees_at_min: -55, degrees_at_max: 55` calibration reads.
/// Measuring it the other way round is a mirror image: every magnitude is
/// identical and every direction is backwards, so it survives any test that
/// compares absolute angles.
[[nodiscard]] float bearingDegrees(float fromX, float fromZ, float toX, float toZ);

/// Elevation from one point to another, in degrees: 0 = level, positive = the
/// target is above the viewer.
[[nodiscard]] float elevationDegrees(float fromX, float fromY, float fromZ, float toX, float toY, float toZ);

/// Compress an angle into an axis's reachable range instead of hard-clamping.
/// `centre` is the midpoint of the axis's sweep and `range` its half-width.
/// Near the centre this is close to linear; far outside it approaches the
/// limit asymptotically, so two far-apart targets stay slightly distinguishable
/// rather than both railing to the identical byte. Returns `centre` when the
/// range is degenerate.
[[nodiscard]] float softClip(float angle, float centre, float range);

/// Map an angle onto the input's 0-255 range using its calibration, clamped.
/// A reversed axis needs no special case: degreesAtMin > degreesAtMax simply
/// produces a descending map.
[[nodiscard]] uint8_t angleToByte(const ResolvedGazeAxis &axis, float angle);

// ---------------------------------------------------------------------------
// Timeline + track construction
// ---------------------------------------------------------------------------

/// Derive who holds the floor over each stretch of the scene from the same
/// per-creature mouth-byte streams the track builder already computes. A
/// non-zero mouth byte means that creature is being voiced.
///
/// Runs are merged across gaps up to `gapToleranceFrames` so ordinary
/// between-word silence doesn't shred one turn into dozens of spans. When two
/// creatures overlap, the one who started more recently wins the floor — an
/// interruption reads as the interrupter taking over.
[[nodiscard]] std::vector<SpeakerSpan>
buildSpeakerTimeline(const std::vector<std::string> &creatureIds,
                     const std::vector<std::span<const uint8_t>> &mouthBytesPerCreature, std::size_t totalFrames,
                     std::size_t gapToleranceFrames);

/// Build one creature's head-aiming byte streams.
///
/// Gaze targets: a speaker plays to the audience — the listener at the origin —
/// while everyone else watches whoever currently holds the floor. Once the
/// floor has been quiet for `neutralReturnHoldMs`, heads ease back to a neutral
/// head-forward pose; short pauses between sentences are ridden out on the last
/// target so nobody twitches home and back mid-conversation.
///
/// `rng` is taken by reference and consumed in a fixed order, so seeding it
/// identically reproduces the result exactly. That is what makes re-rendering
/// a scene against a moved stage change only the gaze and nothing else.
[[nodiscard]] GazeTrack buildGazeTrack(const GazeGeometry &self, const std::vector<GazeGeometry> &others,
                                       const std::vector<SpeakerSpan> &timeline, std::size_t totalFrames,
                                       uint32_t msPerFrame, std::mt19937 &rng, const GazeOptions &options = {});

} // namespace creatures::voice
