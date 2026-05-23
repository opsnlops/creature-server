
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "model/DmxFixture.h"
#include "server/namespace-stuffs.h"

namespace creatures {

/**
 * Phase of an in-flight pattern.
 */
enum class FixturePatternPhase {
    FadeIn,
    Hold,
    FadeOut,
    Done,
};

/**
 * Snapshot of a pattern that's currently being rendered for a fixture.
 *
 * The runner keeps at most one ActivePattern per fixture. Starting a new pattern on a
 * fixture that already has one is a "smooth handoff": the current rendered bytes become
 * the new `startValues`, so there is never a DMX snap.
 */
struct ActivePattern {
    fixtureId_t fixtureId;
    std::string patternId;
    universe_t universe;
    framenum_t startedAtFrame{0};
    framenum_t fadeInDoneFrame{0};
    framenum_t holdDoneFrame{0}; // FRAMENUM_MAX = "hold until explicit stop"
    framenum_t fadeOutDoneFrame{0};
    FixturePatternPhase phase{FixturePatternPhase::FadeIn};

    uint16_t channelOffset{0};
    uint16_t channelSpan{0};

    // Per-channel start/target values (size == channelSpan).
    std::vector<uint8_t> startValues;
    std::vector<uint8_t> targetValues;

    // Most recently rendered values — used as the seed for the next pattern's startValues if
    // another pattern fires while this one is in flight.
    std::vector<uint8_t> lastRenderedValues;

    creatureId_t creatureId; // which trigger started this (may be empty for manual triggers)
    uint32_t fadeInMs{0};
    uint32_t fadeOutMs{0};
    uint32_t holdMs{0};

    // Trace context captured at start() time so DMX frames emitted on later ticks can be
    // linked back in Honeycomb to the originating request span (REST trigger or activity
    // transition). Both empty for triggers without a parent span. See OTel review P1b.
    std::string triggerTraceId;
    std::string triggerSpanId;
};

/**
 * Live-control state for a fixture: raw DMX channel values driven directly by a REST
 * client (slider UI, etc.), with an auto-blackout deadline so a disconnected client
 * doesn't leave the light stuck.
 *
 * Mutually exclusive with `ActivePattern` for the same fixture — live wins. While an
 * `ActiveLive` exists, `start()` refuses to install a new pattern; on expiry, the
 * channels black out and the entry is removed (after which patterns can fire normally).
 */
struct ActiveLive {
    fixtureId_t fixtureId;
    universe_t universe;
    uint16_t channelOffset;
    uint16_t channelSpan;
    // Current per-channel values. Channels not in any incoming setLive() call default
    // to 0 on creation and hold their previous value on subsequent calls.
    std::vector<uint8_t> values;
    framenum_t deadlineFrame{0};
    // Trace context from the originating REST request — surfaced on the tick span so
    // Honeycomb can pivot live frames back to the request that drove them.
    std::string triggerTraceId;
    std::string triggerSpanId;
};

/**
 * Renders fixture patterns into DMX output over time.
 *
 * The runner is passive: it owns the active-pattern map but does no scheduling itself.
 * `FixturePatternTickEvent` (~50 Hz) drives `tick()` from the event loop. The
 * `FixtureBindingDispatcher` arms a tick if one isn't already pending whenever it starts
 * a pattern.
 */
class FixturePatternRunner {
  public:
    FixturePatternRunner() = default;

    /**
     * Begin (or replace) a pattern on a fixture.
     *
     * If a pattern is already active on this fixture, the new pattern uses the currently
     * rendered DMX values as its `startValues` — guarantees no snap.
     *
     * @param parentSpan optional parent for the operation span this method creates.
     * @return false if the pattern doesn't reference any channels on the fixture
     */
    bool start(const DmxFixture &fixture, const FixturePattern &pattern, universe_t universe,
               const creatureId_t &creatureId, framenum_t currentFrame,
               std::shared_ptr<class OperationSpan> parentSpan = nullptr);

    /**
     * Stop a pattern. Transitions it into FadeOut; the entry is removed from the map
     * once fade-out completes.
     */
    void stop(const fixtureId_t &fixtureId, framenum_t currentFrame,
              std::shared_ptr<class OperationSpan> parentSpan = nullptr);

    /**
     * Drive a fixture's channels directly with raw DMX values. Cancels any active
     * pattern on this fixture immediately (no fade-out). Values for channels not
     * named in `channelValues` default to 0 on first call and hold their previous
     * value on subsequent calls within the same live session.
     *
     * @param channelValues per-channel (name → 0..255) updates. All channel names
     *                     must exist on the fixture; otherwise returns false.
     * @param timeoutMs    auto-blackout deadline in ms from `currentFrame`. Must be > 0;
     *                     caller is responsible for clamping to a sane ceiling.
     * @return false if validation failed (unknown channel name, empty values, etc.)
     */
    bool setLive(const DmxFixture &fixture, const std::vector<std::pair<std::string, uint8_t>> &channelValues,
                 uint32_t timeoutMs, universe_t universe, framenum_t currentFrame,
                 std::shared_ptr<class OperationSpan> parentSpan = nullptr);

    /**
     * Inspect whether a fixture is currently under live control. Useful for callers
     * deciding whether a new binding/manual-trigger pattern should be allowed.
     */
    bool hasLive(const fixtureId_t &fixtureId) const;

    /**
     * Advance all active patterns and schedule one DMXEvent per fixture for the current frame.
     *
     * @param tickSpan optional span for recording per-tick counters (active count, emitted
     *                 DMX events, fixtures finished, etc.). Treated as parent for emitted
     *                 DMXEvents' linkage — see FixturePatternTickEvent.
     * @return true if any patterns are still active (caller should reschedule the tick)
     */
    bool tick(framenum_t currentFrame, std::shared_ptr<class OperationSpan> tickSpan = nullptr);

    /**
     * Atomic flag the tick event uses to avoid double-arming itself.
     */
    bool tryArm() {
        std::lock_guard<std::mutex> lock(armMutex_);
        if (tickArmed_)
            return false;
        tickArmed_ = true;
        return true;
    }

    /**
     * Tick event calls this when it's about to execute, so the next start() can re-arm.
     */
    void disarm() {
        std::lock_guard<std::mutex> lock(armMutex_);
        tickArmed_ = false;
    }

    bool hasActivePatterns() const {
        std::lock_guard<std::mutex> lock(mapMutex_);
        return !active_.empty();
    }

    /**
     * Linear-interpolate a single DMX byte. Public for testability — pure function with no
     * runner state. Defined inline so the test binary can call it without linking the rest
     * of the fixture/eventloop subsystem.
     */
    static uint8_t lerp(uint8_t a, uint8_t b, double t) {
        if (t <= 0.0)
            return a;
        if (t >= 1.0)
            return b;
        const double result = static_cast<double>(a) + (static_cast<double>(b) - static_cast<double>(a)) * t;
        if (result < 0.0)
            return 0;
        if (result > 255.0)
            return 255;
        return static_cast<uint8_t>(result + 0.5);
    }

  private:
    mutable std::mutex mapMutex_;
    std::unordered_map<fixtureId_t, ActivePattern> active_;
    std::unordered_map<fixtureId_t, ActiveLive> live_;

    mutable std::mutex armMutex_;
    bool tickArmed_{false};
};

} // namespace creatures
