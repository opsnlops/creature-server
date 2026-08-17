#include "GazeTrack.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <unordered_map>

namespace creatures::voice {

namespace {

constexpr float kRadToDeg = 180.0f / std::numbers::pi_v<float>;

/// Smooth acceleration in, smooth deceleration out, over t in [0, 1]. A linear
/// ramp reads as robotic no matter how well the timing is jittered.
float smoothstep(float t) {
    const float clamped = std::clamp(t, 0.0f, 1.0f);
    return clamped * clamped * (3.0f - 2.0f * clamped);
}

std::size_t msToFrames(uint32_t ms, uint32_t msPerFrame) {
    if (msPerFrame == 0) {
        return 0;
    }
    return static_cast<std::size_t>((ms + msPerFrame - 1) / msPerFrame); // round up; never zero-length
}

float drawFloat(std::mt19937 &rng, float lo, float hi) {
    if (hi <= lo) {
        return lo;
    }
    std::uniform_real_distribution<float> dist(lo, hi);
    return dist(rng);
}

uint32_t drawUint(std::mt19937 &rng, uint32_t lo, uint32_t hi) {
    if (hi <= lo) {
        return lo;
    }
    std::uniform_int_distribution<uint32_t> dist(lo, hi);
    return dist(rng);
}

/// The per-axis animation state used while filling the angle stream.
struct AxisMotion {
    float held{0.0f};       // angle currently being held
    std::size_t start{0};   // first frame of the sweep
    std::size_t arrive{0};  // frame the overshoot peak is reached
    std::size_t settled{0}; // frame the target is finally held
    float from{0.0f};       // angle at `start`
    float target{0.0f};     // angle finally held
    float peak{0.0f};       // target + overshoot
    bool active{false};
};

/// Evaluate one axis at a frame: sweep out to the overshoot peak, settle back
/// to the target, then hold.
float evaluateAxis(const AxisMotion &motion, std::size_t frame) {
    if (!motion.active || frame < motion.start) {
        return motion.held;
    }
    if (frame < motion.arrive) {
        const auto span = static_cast<float>(motion.arrive - motion.start);
        const auto t = static_cast<float>(frame - motion.start) / std::max(span, 1.0f);
        return motion.from + (motion.peak - motion.from) * smoothstep(t);
    }
    if (frame < motion.settled) {
        const auto span = static_cast<float>(motion.settled - motion.arrive);
        const auto t = static_cast<float>(frame - motion.arrive) / std::max(span, 1.0f);
        return motion.peak + (motion.target - motion.peak) * smoothstep(t);
    }
    return motion.target;
}

} // namespace

float bearingDegrees(float fromX, float fromZ, float toX, float toZ) {
    const float dx = toX - fromX;
    const float dz = toZ - fromZ;
    if (dx == 0.0f && dz == 0.0f) {
        return 0.0f;
    }
    // Note the NEGATED dx (issue #144). Zero points along +Z and angles grow
    // toward -X, which is the same rotational sense the Console uses for a
    // placement's `yaw` — so the two can simply be subtracted. It also makes a
    // positive result mean "turn to your right", which is what a hand-written
    // `degrees_at_min: -55, degrees_at_max: 55` calibration naturally reads as.
    return std::atan2(-dx, dz) * kRadToDeg;
}

float elevationDegrees(float fromX, float fromY, float fromZ, float toX, float toY, float toZ) {
    const float dx = toX - fromX;
    const float dy = toY - fromY;
    const float dz = toZ - fromZ;
    const float horizontal = std::sqrt(dx * dx + dz * dz);
    if (horizontal == 0.0f && dy == 0.0f) {
        return 0.0f;
    }
    return std::atan2(dy, horizontal) * kRadToDeg;
}

float softClip(float angle, float centre, float range) {
    if (!(range > 0.0f) || !std::isfinite(angle)) {
        return centre;
    }
    return centre + range * std::tanh((angle - centre) / range);
}

uint8_t angleToByte(const ResolvedGazeAxis &axis, float angle) {
    const float sweep = axis.degreesAtMax - axis.degreesAtMin;
    if (sweep == 0.0f || !std::isfinite(angle)) {
        // A zero-width sweep is rejected at parse time; treat it as centre
        // rather than dividing by zero if one ever reaches us anyway.
        return 128;
    }
    const float normalized = (angle - axis.degreesAtMin) / sweep;
    const float scaled = std::round(normalized * 255.0f);
    return static_cast<uint8_t>(std::clamp(scaled, 0.0f, 255.0f));
}

GazeGeometry resolveGazeGeometry(const creatures::Creature &creature, const creatures::StagePlacement &placement) {
    GazeGeometry geometry;
    geometry.creatureId = creature.id;
    geometry.x = placement.x;
    geometry.y = placement.y;
    geometry.z = placement.z;
    geometry.yaw = placement.yaw;

    if (!creature.gaze) {
        return geometry;
    }

    // The name-to-slot lookup. This is why the config names an input instead
    // of numbering a slot: the two creature families lay their inputs out
    // differently, so the same "neck_rotate" is slot 2 on one bird and slot 5
    // on another.
    auto resolve = [&](const std::optional<creatures::GazeAxis> &configured, std::optional<ResolvedGazeAxis> &target) {
        if (!configured) {
            return;
        }
        const auto it = std::find_if(creature.inputs.begin(), creature.inputs.end(),
                                     [&](const creatures::Input &i) { return i.name == configured->input; });
        if (it == creature.inputs.end()) {
            // Rejected at upload time, so reaching here means a stored config
            // drifted. Drop the axis rather than guess at a slot.
            return;
        }
        ResolvedGazeAxis resolved;
        resolved.slot = it->slot;
        resolved.degreesAtMin = configured->degrees_at_min;
        resolved.degreesAtMax = configured->degrees_at_max;
        resolved.listeningAmount = configured->listening_amount;
        target = resolved;
    };

    resolve(creature.gaze->pan, geometry.pan);
    resolve(creature.gaze->elevation, geometry.elevation);
    resolve(creature.gaze->cock, geometry.cock);
    return geometry;
}

std::vector<SpeakerSpan> buildSpeakerTimeline(const std::vector<std::string> &creatureIds,
                                              const std::vector<std::span<const uint8_t>> &mouthBytesPerCreature,
                                              std::size_t totalFrames, std::size_t gapToleranceFrames) {
    std::vector<SpeakerSpan> spans;
    if (creatureIds.size() != mouthBytesPerCreature.size()) {
        return spans;
    }

    // Per creature, collapse the mouth-byte stream into runs, merging gaps
    // shorter than the tolerance so between-word silence doesn't fragment a
    // single turn into dozens of spans.
    for (std::size_t c = 0; c < creatureIds.size(); ++c) {
        const auto &mouth = mouthBytesPerCreature[c];
        std::size_t runStart = std::numeric_limits<std::size_t>::max();
        std::size_t lastVoiced = 0;

        for (std::size_t f = 0; f <= totalFrames; ++f) {
            const bool voiced = f < totalFrames && f < mouth.size() && mouth[f] != 0;
            if (voiced) {
                if (runStart == std::numeric_limits<std::size_t>::max()) {
                    runStart = f;
                }
                lastVoiced = f;
                continue;
            }
            const bool runOpen = runStart != std::numeric_limits<std::size_t>::max();
            const bool gapExceeded = runOpen && (f - lastVoiced) > gapToleranceFrames;
            if (runOpen && (gapExceeded || f == totalFrames)) {
                spans.push_back(SpeakerSpan{runStart, lastVoiced + 1, creatureIds[c]});
                runStart = std::numeric_limits<std::size_t>::max();
            }
        }
    }

    // Later starts win the floor, so an interruption reads as the interrupter
    // taking over rather than being ignored until the first speaker finishes.
    std::sort(spans.begin(), spans.end(),
              [](const SpeakerSpan &a, const SpeakerSpan &b) { return a.startFrame < b.startFrame; });
    return spans;
}

GazeTrack buildGazeTrack(const GazeGeometry &self, const std::vector<GazeGeometry> &others,
                         const std::vector<SpeakerSpan> &timeline, std::size_t totalFrames, uint32_t msPerFrame,
                         std::mt19937 &rng, const GazeOptions &options) {
    GazeTrack track;
    if (totalFrames == 0 || msPerFrame == 0 || (!self.pan && !self.elevation && !self.cock)) {
        return track;
    }

    std::unordered_map<std::string, const GazeGeometry *> byId;
    byId.reserve(others.size());
    for (const auto &other : others) {
        if (other.creatureId != self.creatureId) {
            byId.emplace(other.creatureId, &other);
        }
    }

    // Resolve "look at X" into angles this creature can actually reach.
    const float panCentre = self.pan ? self.pan->centre() : 0.0f;
    const float panRange = self.pan ? self.pan->range() : 0.0f;
    const float elevationCentre = self.elevation ? self.elevation->centre() : 0.0f;
    const float elevationRange = self.elevation ? self.elevation->range() : 0.0f;
    const float cockCentre = self.cock ? self.cock->centre() : 0.0f;
    const float cockRange = self.cock ? self.cock->range() : 0.0f;

    auto aimAt = [&](float targetX, float targetY, float targetZ, float &panOut, float &elevationOut) {
        // bearingDegrees and `yaw` are deliberately in the SAME rotational
        // sense (issue #144), so this is a plain subtraction. They used not to
        // be, and mixing the two mirrored frames is what pinned every neck to a
        // mechanical rail while still looking entirely reasonable in review.
        const float bearing = bearingDegrees(self.x, self.z, targetX, targetZ);
        const float relative = creatures::normalizeDegrees(bearing - self.yaw);
        panOut = softClip(relative, panCentre, panRange);
        const float elevation = elevationDegrees(self.x, self.y, self.z, targetX, targetY, targetZ);
        elevationOut = softClip(elevation, elevationCentre, elevationRange);
    };

    // The listener at the origin is a valid target, and is what the first
    // speaker of a scene plays to.
    float listenerPan = 0.0f;
    float listenerElevation = 0.0f;
    aimAt(0.0f, 0.0f, 0.0f, listenerPan, listenerElevation);

    auto aimAtCreature = [&](const std::string &creatureId, float &panOut, float &elevationOut) -> bool {
        auto it = byId.find(creatureId);
        if (it == byId.end()) {
            return false;
        }
        aimAt(it->second->x, it->second->y, it->second->z, panOut, elevationOut);
        return true;
    };

    // Walk the timeline building the sequence of target changes. A speaker
    // plays to the audience; listeners watch whoever holds the floor; when the
    // floor goes quiet for long enough, everyone eases back to a neutral,
    // head-forward pose rather than holding their last target indefinitely.
    struct GazeSegmentLocal {
        std::size_t changeFrame{0};
        float panAngle{0.0f};
        float elevationAngle{0.0f};
        float cockAngle{0.0f};
        // A relax-to-neutral rather than an aim. Moves slower and never
        // overshoots — settling back is not a reaction to anything.
        bool neutral{false};
    };
    // Which way this creature favours cocking its head. Drawn once per scene,
    // before anything else touches the rng, so it stays stable and the stream
    // position remains deterministic.
    const float preferredCockSide = drawFloat(rng, 0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f;

    std::vector<GazeSegmentLocal> segments;
    segments.push_back(GazeSegmentLocal{0, listenerPan, listenerElevation, cockCentre});

    // How long the floor must stay quiet before heads drift home, and how far
    // into that silence the move begins.
    const auto neutralHoldFrames = msToFrames(options.neutralReturnHoldMs, msPerFrame);

    // Insert a relax-to-neutral segment if the floor has been quiet since
    // `quietSince` for longer than the hold. `quietSince` is the end of the
    // last span anyone was speaking in.
    // Roughly how long the unwind itself takes. Used two ways: a mid-scene gap
    // has to be long enough to actually finish one (a settle that gets yanked
    // back after two frames IS the jerk we're removing), and the end-of-scene
    // settle has to start early enough to complete before the track runs out.
    const auto neutralTravelFrames =
        msToFrames(static_cast<uint32_t>(std::lround(options.maxPanTravelMs * options.neutralReturnTravelScale)) +
                       options.minReactionMs + options.settleMs,
                   msPerFrame);

    auto pushNeutral = [&](std::size_t at) {
        const auto &last = segments.back();
        if (std::abs(last.panAngle - panCentre) < 0.5f && std::abs(last.elevationAngle - elevationCentre) < 0.5f &&
            std::abs(last.cockAngle - cockCentre) < 0.5f) {
            return; // already neutral
        }
        if (!segments.empty() && at <= segments.back().changeFrame) {
            return; // would land on or before the move already in flight
        }
        segments.push_back(GazeSegmentLocal{at, panCentre, elevationCentre, cockCentre, true});
    };

    auto considerNeutralReturn = [&](std::size_t quietSince, std::size_t nextChange) {
        if (quietSince == 0) {
            return; // nothing has been said yet; the opening pose stands
        }
        const std::size_t returnAt = quietSince + neutralHoldFrames;
        // Require room to actually complete the unwind before the next turn,
        // otherwise the head starts home and is immediately hauled back.
        if (returnAt + neutralTravelFrames >= nextChange || returnAt >= totalFrames) {
            return;
        }
        pushNeutral(returnAt);
    };

    // Past this point there isn't room to complete a head move before the track
    // ends, so no new aim is worth starting — the only thing left to do is wind
    // down. Without this, a turn (or a fresh head-cock) landing in the last
    // second leaves the head committed to a target it never reaches, and blocks
    // the settle that makes the handoff to the idle loop smooth.
    const std::size_t latestUsefulStart = totalFrames > neutralTravelFrames ? totalFrames - neutralTravelFrames : 0;

    std::size_t quietSince = 0; // frame the floor last went quiet
    for (const auto &span : timeline) {
        float panAngle = 0.0f;
        float elevationAngle = 0.0f;
        bool resolved = false;
        const bool selfIsSpeaking = span.creatureId == self.creatureId;

        // Relax toward neutral across a long gap before this turn opens.
        considerNeutralReturn(quietSince, span.startFrame);
        quietSince = std::max(quietSince, span.endFrame);

        if (selfIsSpeaking) {
            // We're the one talking, so play to the house. (This used to aim at
            // whoever spoke previously, which reads as two birds muttering at
            // each other rather than performing to a room.)
            panAngle = listenerPan;
            elevationAngle = listenerElevation;
            resolved = true;
        } else {
            resolved = aimAtCreature(span.creatureId, panAngle, elevationAngle);
        }

        if (!resolved) {
            continue; // speaker isn't on this stage; hold whatever we had
        }

        // Head-cock: the dog thing. A speaking creature is level — you don't
        // cock your head at someone while you're the one talking. A listening
        // one drops an ear toward its shoulder and, crucially, HOLDS it: a
        // fresh angle on every turn would read as a twitch, not curiosity.
        float cockAngle = cockCentre;
        if (!selfIsSpeaking && cockRange > 0.0f) {
            const float amount = std::clamp(self.cock ? self.cock->listeningAmount : 0.0f, 0.0f, 1.0f);
            const float previousCock = segments.back().cockAngle;
            const bool alreadyCocked = std::abs(previousCock - cockCentre) > 0.5f;

            if (amount <= 0.0f) {
                cockAngle = cockCentre;
            } else if (alreadyCocked && drawFloat(rng, 0.0f, 1.0f) < options.cockPersistence) {
                cockAngle = previousCock; // hold it, like a dog would
            } else if (drawFloat(rng, 0.0f, 1.0f) < options.chanceOfCock) {
                // Mostly to this creature's favoured side, occasionally the
                // other — a bird that only ever tilts one way looks broken,
                // one that alternates at random looks confused.
                const bool useFavoured = drawFloat(rng, 0.0f, 1.0f) < options.preferredSideBias;
                const float side = useFavoured ? preferredCockSide : -preferredCockSide;
                const float magnitude = drawFloat(rng, cockRange * amount * 0.4f, cockRange * amount);
                cockAngle = cockCentre + side * magnitude;
            }
        }

        const auto &last = segments.back();
        // Sub-degree changes aren't worth a whole head move.
        if (std::abs(panAngle - last.panAngle) < 0.5f && std::abs(elevationAngle - last.elevationAngle) < 0.5f &&
            std::abs(cockAngle - last.cockAngle) < 0.5f) {
            continue;
        }
        // Not everyone reacts to every turn. Drawing this per transition (and
        // per creature, since each has its own rng stream position) is what
        // keeps the group from moving as one organism.
        if (drawFloat(rng, 0.0f, 1.0f) < options.ignoreTurnChance) {
            continue;
        }
        // Too late in the scene to be worth starting; wind down instead. The
        // rng draws above still happened, so the stream stays deterministic.
        if (span.startFrame >= latestUsefulStart) {
            continue;
        }
        segments.push_back(GazeSegmentLocal{span.startFrame, panAngle, elevationAngle, cockAngle, false});
    }

    // The end of the scene is the case that actually matters, and it is NOT
    // just "a long enough gap". A dialog usually stops within a fraction of a
    // second of the last word, so the ordinary hold never elapses and the head
    // would be left held hard over on its final target. Playback then hands off
    // to an idle loop whose head is somewhere else entirely, and the creature
    // snaps its neck across the difference.
    //
    // So the tail settle is unconditional, and it starts late enough to look
    // like winding down but early enough to actually arrive — even if that
    // means beginning while the last few words are still being said.
    if (quietSince > 0 && totalFrames > 0) {
        pushNeutral(std::min(quietSince + neutralHoldFrames, latestUsefulStart));
    }

    // Per-creature drift phase and period, drawn once so the wander is stable
    // across the whole scene rather than re-randomizing every frame.
    const float panDriftPhase = drawFloat(rng, 0.0f, 2.0f * std::numbers::pi_v<float>);
    const float elevationDriftPhase = drawFloat(rng, 0.0f, 2.0f * std::numbers::pi_v<float>);
    const auto panDriftFrames = std::max<std::size_t>(
        1, msToFrames(drawUint(rng, options.minDriftPeriodMs, options.maxDriftPeriodMs), msPerFrame));
    const auto elevationDriftFrames = std::max<std::size_t>(
        1, msToFrames(drawUint(rng, options.minDriftPeriodMs, options.maxDriftPeriodMs), msPerFrame));

    const auto settleFrames = std::max<std::size_t>(1, msToFrames(options.settleMs, msPerFrame));

    AxisMotion panMotion;
    AxisMotion elevationMotion;
    AxisMotion cockMotion;
    panMotion.held = panMotion.target = segments.front().panAngle;
    elevationMotion.held = elevationMotion.target = segments.front().elevationAngle;
    cockMotion.held = cockMotion.target = segments.front().cockAngle;

    // Pre-compute each transition's timing. All draws happen here, in segment
    // order, so the rng stream is deterministic for a given seed.
    struct PlannedMove {
        std::size_t start{0};
        std::size_t panArrive{0}, panSettled{0};
        std::size_t elevationArrive{0}, elevationSettled{0};
        std::size_t cockArrive{0}, cockSettled{0};
        float panOvershoot{0.0f};
        float elevationOvershoot{0.0f};
    };
    std::vector<PlannedMove> moves;
    moves.reserve(segments.size());

    for (std::size_t i = 1; i < segments.size(); ++i) {
        const auto &segment = segments[i];
        // One reaction delay shared by every axis — a head starts moving as a
        // head — but slightly different travel times, so the gaze traces a
        // curve instead of a straight diagonal.
        auto reactionFrames = msToFrames(drawUint(rng, options.minReactionMs, options.maxReactionMs), msPerFrame);
        auto panTravelMs = drawUint(rng, options.minPanTravelMs, options.maxPanTravelMs);
        const float elevationScale = drawFloat(rng, options.elevationTravelScaleMin, options.elevationTravelScaleMax);
        float overshoot = drawFloat(rng, options.minOvershootDegrees, options.maxOvershootDegrees);

        // Settling home doesn't overshoot, and it doesn't get a reaction delay
        // either — there is no cue to react to. Budgeting the full draw here is
        // what made the wind-down start uselessly early and cut short the time
        // creatures spent actually looking at each other.
        //
        // The draws above still happen unconditionally so the rng stream stays
        // in lockstep regardless of where the neutral returns land.
        if (segment.neutral) {
            panTravelMs = static_cast<uint32_t>(std::lround(panTravelMs * options.neutralReturnTravelScale));
            reactionFrames = msToFrames(options.minReactionMs, msPerFrame);
            overshoot = 0.0f;
        }

        const auto panTravelFrames = std::max<std::size_t>(1, msToFrames(panTravelMs, msPerFrame));
        const auto elevationTravelFrames = std::max<std::size_t>(
            1, msToFrames(static_cast<uint32_t>(std::lround(panTravelMs * elevationScale)), msPerFrame));

        PlannedMove move;
        move.start = segment.changeFrame + reactionFrames;
        move.panArrive = move.start + panTravelFrames;
        move.panSettled = move.panArrive + settleFrames;
        move.elevationArrive = move.start + elevationTravelFrames;
        move.elevationSettled = move.elevationArrive + settleFrames;
        // The cock is a slow, deliberate lean — no overshoot, and it takes its
        // time. A snappy head-cock reads as a twitch rather than curiosity.
        move.cockArrive = move.start + panTravelFrames * 2;
        move.cockSettled = move.cockArrive;
        move.panOvershoot = overshoot;
        move.elevationOvershoot = overshoot * 0.5f;
        moves.push_back(move);
    }

    if (self.pan) {
        track.panBytes.resize(totalFrames);
        track.panSlot = self.pan->slot;
    }
    if (self.elevation) {
        track.elevationBytes.resize(totalFrames);
        track.elevationSlot = self.elevation->slot;
    }
    if (self.cock) {
        track.cockBytes.resize(totalFrames);
        track.cockSlot = self.cock->slot;
    }

    auto arm = [](AxisMotion &motion, std::size_t frame, std::size_t start, std::size_t arrive, std::size_t settled,
                  float target, float overshoot) {
        const float now = evaluateAxis(motion, frame);
        motion.held = now;
        motion.from = now;
        motion.start = std::max(start, frame);
        motion.arrive = motion.start + (arrive - start);
        motion.settled = motion.arrive + (settled - arrive);
        motion.target = target;
        motion.peak = target + std::copysign(overshoot, target - now);
        motion.active = true;
    };

    std::size_t nextMove = 0;
    for (std::size_t f = 0; f < totalFrames; ++f) {
        // Arm the next move when its change frame arrives. A move that lands
        // while an earlier one is still running simply replaces it, starting
        // from wherever the head currently is — which is what interrupting a
        // turn should look like.
        while (nextMove < moves.size() && segments[nextMove + 1].changeFrame <= f) {
            const auto &move = moves[nextMove];
            const auto &segment = segments[nextMove + 1];
            arm(panMotion, f, move.start, move.panArrive, move.panSettled, segment.panAngle, move.panOvershoot);
            arm(elevationMotion, f, move.start, move.elevationArrive, move.elevationSettled, segment.elevationAngle,
                move.elevationOvershoot);
            arm(cockMotion, f, move.start, move.cockArrive, move.cockSettled, segment.cockAngle, 0.0f);
            ++nextMove;
        }

        const auto drift = [&](float amplitude, std::size_t periodFrames, float phase) {
            return amplitude * std::sin(2.0f * std::numbers::pi_v<float> * static_cast<float>(f) /
                                            static_cast<float>(periodFrames) +
                                        phase);
        };

        if (self.pan) {
            track.panBytes[f] = angleToByte(
                *self.pan, evaluateAxis(panMotion, f) + drift(options.panDriftDegrees, panDriftFrames, panDriftPhase));
        }
        if (self.elevation) {
            track.elevationBytes[f] = angleToByte(
                *self.elevation, evaluateAxis(elevationMotion, f) +
                                     drift(options.elevationDriftDegrees, elevationDriftFrames, elevationDriftPhase));
        }
        if (self.cock) {
            // No drift on the cock — a wandering head tilt looks like a fault,
            // not like life.
            track.cockBytes[f] = angleToByte(*self.cock, evaluateAxis(cockMotion, f));
        }
    }

    return track;
}

} // namespace creatures::voice
