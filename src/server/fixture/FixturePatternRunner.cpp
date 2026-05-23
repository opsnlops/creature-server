
#include "FixturePatternRunner.h"

#include <algorithm>
#include <limits>
#include <memory>

#include <spdlog/spdlog.h>

#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;

bool FixturePatternRunner::start(const DmxFixture &fixture, const FixturePattern &pattern, universe_t universe,
                                 const creatureId_t &creatureId, framenum_t currentFrame,
                                 std::shared_ptr<OperationSpan> parentSpan) {

    auto span =
        observability ? observability->createChildOperationSpan("FixturePatternRunner.start", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixture.id);
        span->setAttribute("fixture.name", fixture.name);
        span->setAttribute("fixture.universe", static_cast<int64_t>(universe));
        span->setAttribute("pattern.id", pattern.id);
        span->setAttribute("pattern.name", pattern.name);
        span->setAttribute("pattern.fade_in_ms", static_cast<int64_t>(pattern.fade_in_ms));
        span->setAttribute("pattern.hold_ms", static_cast<int64_t>(pattern.hold_ms));
        span->setAttribute("pattern.fade_out_ms", static_cast<int64_t>(pattern.fade_out_ms));
        span->setAttribute("creature.id", creatureId);
    }

    if (fixture.channels.empty()) {
        warn("FixturePatternRunner::start: fixture {} has no channels", fixture.id);
        if (span) {
            span->setAttribute("error.type", "NoChannels");
            span->setError("fixture has no channels");
        }
        return false;
    }

    // Live control wins. If a slider is driving this fixture, refuse to install a new
    // pattern — let the human operator finish before bindings or manual triggers fight
    // them for the channels. Future pattern triggers work fine once live expires.
    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        if (live_.find(fixture.id) != live_.end()) {
            debug("FixturePatternRunner::start: fixture {} is under live control, refusing pattern {}", fixture.id,
                  pattern.id);
            if (span) {
                span->setAttribute("error.type", "FixtureUnderLiveControl");
                span->setError("fixture is currently under live control");
            }
            return false;
        }
    }

    // Compute the span the pattern occupies (max channel offset + 1).
    uint16_t maxOffset = 0;
    for (const auto &ch : fixture.channels) {
        maxOffset = std::max(maxOffset, ch.offset);
    }
    const uint16_t channelSpan = static_cast<uint16_t>(maxOffset + 1);

    // Build the target value buffer at the per-fixture channel offsets the pattern touches.
    std::vector<uint8_t> targetValues(channelSpan, 0);
    bool anyChannelTouched = false;
    for (const auto &pv : pattern.values) {
        const FixtureChannel *channel = fixture.findChannelByName(pv.channel);
        if (!channel)
            continue;
        targetValues[channel->offset] = pv.value;
        anyChannelTouched = true;
    }

    if (!anyChannelTouched) {
        warn("FixturePatternRunner::start: pattern {} on fixture {} touched no valid channels", pattern.id, fixture.id);
        if (span) {
            span->setAttribute("error.type", "NoChannelsTouched");
            span->setError("pattern touched no valid channels");
        }
        return false;
    }

    ActivePattern entry;
    entry.fixtureId = fixture.id;
    entry.patternId = pattern.id;
    entry.universe = universe;
    entry.startedAtFrame = currentFrame;
    entry.fadeInMs = pattern.fade_in_ms;
    entry.fadeOutMs = pattern.fade_out_ms;
    entry.holdMs = pattern.hold_ms;
    entry.channelOffset = fixture.channel_offset;
    entry.channelSpan = channelSpan;
    entry.targetValues = std::move(targetValues);
    entry.phase = FixturePatternPhase::FadeIn;
    entry.creatureId = creatureId;
    // Capture the originating trace context so later tick spans can link back to the
    // REST trigger / activity transition that started this pattern.
    if (parentSpan) {
        entry.triggerTraceId = parentSpan->getTraceIdHex();
        entry.triggerSpanId = parentSpan->getSpanIdHex();
    }

    entry.fadeInDoneFrame = entry.startedAtFrame + static_cast<framenum_t>(entry.fadeInMs);
    if (entry.holdMs == 0) {
        entry.holdDoneFrame = std::numeric_limits<framenum_t>::max();
    } else {
        entry.holdDoneFrame = entry.fadeInDoneFrame + static_cast<framenum_t>(entry.holdMs);
    }
    entry.fadeOutDoneFrame = entry.holdDoneFrame == std::numeric_limits<framenum_t>::max()
                                 ? std::numeric_limits<framenum_t>::max()
                                 : (entry.holdDoneFrame + static_cast<framenum_t>(entry.fadeOutMs));

    {
        std::lock_guard<std::mutex> lock(mapMutex_);
        auto it = active_.find(fixture.id);
        if (it != active_.end()) {
            // Smooth handoff — seed startValues from the currently rendered bytes.
            entry.startValues = it->second.lastRenderedValues.empty() ? std::vector<uint8_t>(channelSpan, 0)
                                                                      : it->second.lastRenderedValues;
            // Pad/trim to the new span.
            entry.startValues.resize(channelSpan, 0);
        } else {
            entry.startValues.assign(channelSpan, 0);
        }
        entry.lastRenderedValues = entry.startValues;
        active_[fixture.id] = std::move(entry);
    }

    debug(
        "FixturePatternRunner: started pattern {} on fixture {} (universe {}, fade_in={}ms, hold={}ms, fade_out={}ms)",
        pattern.id, fixture.id, universe, pattern.fade_in_ms, pattern.hold_ms, pattern.fade_out_ms);
    if (span)
        span->setSuccess();
    return true;
}

void FixturePatternRunner::stop(const fixtureId_t &fixtureId, framenum_t currentFrame,
                                std::shared_ptr<OperationSpan> parentSpan) {
    auto span =
        observability ? observability->createChildOperationSpan("FixturePatternRunner.stop", parentSpan) : nullptr;
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }

    std::lock_guard<std::mutex> lock(mapMutex_);
    auto it = active_.find(fixtureId);
    if (it == active_.end()) {
        if (span) {
            span->setAttribute("fixture.pattern.no_op", "not_active");
            span->setSuccess();
        }
        return;
    }
    auto &entry = it->second;
    if (entry.phase == FixturePatternPhase::FadeOut || entry.phase == FixturePatternPhase::Done) {
        if (span) {
            span->setAttribute("fixture.pattern.no_op", "already_stopping");
            span->setSuccess();
        }
        return;
    }
    if (span) {
        span->setAttribute("pattern.id", entry.patternId);
        span->setAttribute("pattern.fade_out_ms", static_cast<int64_t>(entry.fadeOutMs));
    }

    // Start fading out from wherever we are now. The current rendered values become the new
    // "start" for the fade-out interpolation, going back toward zero (or whatever startValues were).
    entry.startValues = entry.lastRenderedValues.empty() ? entry.targetValues : entry.lastRenderedValues;
    entry.startValues.resize(entry.channelSpan, 0);

    // Reset target to the original pre-pattern values. For now we use zeros, matching
    // "off when not held". A future enhancement could capture the pre-pattern DMX state.
    std::vector<uint8_t> zeros(entry.channelSpan, 0);
    entry.targetValues = std::move(zeros);

    entry.phase = FixturePatternPhase::FadeOut;
    entry.startedAtFrame = currentFrame;
    entry.fadeInDoneFrame = currentFrame;
    entry.holdDoneFrame = currentFrame;
    entry.fadeOutDoneFrame = currentFrame + static_cast<framenum_t>(entry.fadeOutMs);
    debug("FixturePatternRunner: fade-out starting on fixture {} ({}ms)", fixtureId, entry.fadeOutMs);
    if (span)
        span->setSuccess();
}

bool FixturePatternRunner::setLive(const DmxFixture &fixture,
                                   const std::vector<std::pair<std::string, uint8_t>> &channelValues,
                                   uint32_t timeoutMs, universe_t universe, framenum_t currentFrame,
                                   std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("FixturePatternRunner.setLive", parentSpan)
                              : std::shared_ptr<OperationSpan>{};
    if (span) {
        span->setAttribute("fixture.id", fixture.id);
        span->setAttribute("fixture.universe", static_cast<int64_t>(universe));
        span->setAttribute("fixture.live.channel_value_count", static_cast<int64_t>(channelValues.size()));
        span->setAttribute("fixture.live.timeout_ms", static_cast<int64_t>(timeoutMs));
    }

    if (fixture.channels.empty()) {
        warn("FixturePatternRunner::setLive: fixture {} has no channels", fixture.id);
        if (span) {
            span->setAttribute("error.type", "NoChannels");
            span->setError("fixture has no channels");
        }
        return false;
    }
    if (channelValues.empty()) {
        warn("FixturePatternRunner::setLive: fixture {} called with no channel values", fixture.id);
        if (span) {
            span->setAttribute("error.type", "NoChannelValues");
            span->setError("at least one channel value is required");
        }
        return false;
    }
    if (timeoutMs == 0) {
        warn("FixturePatternRunner::setLive: fixture {} timeoutMs must be > 0", fixture.id);
        if (span) {
            span->setAttribute("error.type", "InvalidTimeout");
            span->setError("timeout_ms must be > 0");
        }
        return false;
    }

    // Compute the fixture's channel span — same idiom as start(). Max referenced offset
    // across the fixture's channels (not just the ones being set live) so we keep a
    // consistent address window across calls within a session.
    uint16_t maxOffset = 0;
    for (const auto &ch : fixture.channels) {
        if (ch.offset > maxOffset)
            maxOffset = ch.offset;
    }
    const uint16_t channelSpan = static_cast<uint16_t>(maxOffset + 1);

    // Validate every channel name up front, *before* we touch the live map. Any unknown
    // name is a hard error and we want both no-side-effects and a clean rollback for
    // existing sessions. Resolving offsets in a local table first means we never have
    // to undo a half-applied entry.
    std::vector<std::pair<uint16_t, uint8_t>> resolved;
    resolved.reserve(channelValues.size());
    for (const auto &cv : channelValues) {
        const FixtureChannel *matched = nullptr;
        for (const auto &ch : fixture.channels) {
            if (ch.name == cv.first) {
                matched = &ch;
                break;
            }
        }
        if (!matched) {
            warn("FixturePatternRunner::setLive: fixture {} has no channel named '{}'", fixture.id, cv.first);
            if (span) {
                span->setAttribute("error.type", "UnknownChannel");
                span->setAttribute("error.channel_name", cv.first);
                span->setError("unknown channel name");
            }
            return false;
        }
        if (matched->offset >= channelSpan) {
            warn("FixturePatternRunner::setLive: fixture {} channel '{}' offset {} out of range", fixture.id, cv.first,
                 matched->offset);
            if (span) {
                span->setAttribute("error.type", "ChannelOffsetOutOfRange");
                span->setError("channel offset out of range");
            }
            return false;
        }
        resolved.emplace_back(matched->offset, cv.second);
    }

    std::lock_guard<std::mutex> lock(mapMutex_);

    // Find or create the live entry. New entries seed all channels to 0; subsequent
    // calls within the same live session hold previous values for channels not named.
    auto it = live_.find(fixture.id);
    if (it == live_.end()) {
        ActiveLive fresh;
        fresh.fixtureId = fixture.id;
        fresh.universe = universe;
        fresh.channelOffset = fixture.channel_offset;
        fresh.channelSpan = channelSpan;
        fresh.values.assign(channelSpan, 0);
        it = live_.emplace(fixture.id, std::move(fresh)).first;

        // Live arriving cancels any active pattern hard (no fade-out). The plan calls
        // this out explicitly — live is operator-driven and should not negotiate.
        if (active_.erase(fixture.id) > 0) {
            debug("FixturePatternRunner::setLive: fixture {} had active pattern, cancelled", fixture.id);
            if (span)
                span->setAttribute("fixture.live.cancelled_active_pattern", true);
        }
    } else {
        // Existing live session: keep previous values, refresh universe + channelOffset
        // in case the universe assignment changed mid-session (unlikely but cheap).
        it->second.universe = universe;
        it->second.channelOffset = fixture.channel_offset;
        if (it->second.values.size() != channelSpan) {
            it->second.values.resize(channelSpan, 0);
            it->second.channelSpan = channelSpan;
        }
    }
    auto &entry = it->second;

    for (const auto &rv : resolved) {
        entry.values[rv.first] = rv.second;
    }

    entry.deadlineFrame = currentFrame + static_cast<framenum_t>(timeoutMs);

    // Refresh trigger trace context every call — the most recent slider movement is the
    // most useful link in Honeycomb.
    if (parentSpan) {
        entry.triggerTraceId = parentSpan->getTraceIdHex();
        entry.triggerSpanId = parentSpan->getSpanIdHex();
    } else {
        entry.triggerTraceId.clear();
        entry.triggerSpanId.clear();
    }

    debug("FixturePatternRunner::setLive: fixture {} now driving {} channels (deadline +{}ms)", fixture.id,
          channelValues.size(), timeoutMs);
    if (span) {
        span->setAttribute("fixture.live.channel_span", static_cast<int64_t>(channelSpan));
        span->setSuccess();
    }
    return true;
}

bool FixturePatternRunner::hasLive(const fixtureId_t &fixtureId) const {
    std::lock_guard<std::mutex> lock(mapMutex_);
    return live_.find(fixtureId) != live_.end();
}

bool FixturePatternRunner::tick(framenum_t currentFrame, std::shared_ptr<OperationSpan> tickSpan) {

    // KNOWN LIMITATION: animation-vs-pattern precedence is not enforced here.
    //
    // The plan (plan/dmx-fixture.md → "Precedence: animation tracks vs patterns") says
    // animation tracks should win over fixture patterns when both target the same
    // (universe, channel) range. That is NOT currently implemented — both this tick
    // and PlaybackRunnerEvent::emitDmxFrames enqueue DMXEvents to the same event loop,
    // and the wire value for any given byte is just "whichever event the loop processes
    // last for that frame." Because the pattern tick runs at ~20 ms cadence and the
    // playback runner at 1 ms, animation will usually win by writing more recently —
    // but this is incidental, not guaranteed, and a fixture whose channel range overlaps
    // a creature's animation output can stomp the animation in unpredictable ways.
    //
    // Mitigations (not yet implemented):
    //   1. Cheap: at start() time, refuse to start a pattern on a fixture whose
    //      (universe, channel_offset..+span) overlaps an active PlaybackSession track.
    //   2. Proper: a merge step in the event loop that combines pending DMXEvents
    //      for the same frame, with documented priority.
    //
    // This is tracked in the "DmxFixture follow-up work" section of AGENTS.md.

    std::vector<ActivePattern *> toEmit;
    std::vector<fixtureId_t> finished;

    std::unique_lock<std::mutex> lock(mapMutex_);

    for (auto &kv : active_) {
        auto &entry = kv.second;
        const framenum_t elapsed = currentFrame - entry.startedAtFrame;

        std::vector<uint8_t> rendered(entry.channelSpan, 0);

        switch (entry.phase) {
        case FixturePatternPhase::FadeIn: {
            if (entry.fadeInMs == 0 || currentFrame >= entry.fadeInDoneFrame) {
                rendered = entry.targetValues;
                entry.phase = FixturePatternPhase::Hold;
            } else {
                const double t = static_cast<double>(elapsed) / static_cast<double>(entry.fadeInMs);
                for (size_t i = 0; i < entry.channelSpan; ++i) {
                    const uint8_t a = (i < entry.startValues.size()) ? entry.startValues[i] : 0;
                    const uint8_t b = (i < entry.targetValues.size()) ? entry.targetValues[i] : 0;
                    rendered[i] = lerp(a, b, t);
                }
            }
            break;
        }
        case FixturePatternPhase::Hold: {
            if (entry.holdDoneFrame != std::numeric_limits<framenum_t>::max() && currentFrame >= entry.holdDoneFrame) {
                // Transition into fade-out automatically when hold_ms expires.
                entry.phase = FixturePatternPhase::FadeOut;
                entry.startedAtFrame = currentFrame;
                entry.fadeOutDoneFrame = currentFrame + static_cast<framenum_t>(entry.fadeOutMs);
                entry.startValues = entry.targetValues;
                std::vector<uint8_t> zeros(entry.channelSpan, 0);
                entry.targetValues = std::move(zeros);
                rendered = entry.startValues;
            } else {
                rendered = entry.targetValues;
            }
            break;
        }
        case FixturePatternPhase::FadeOut: {
            const framenum_t fadeElapsed = currentFrame - entry.startedAtFrame;
            if (entry.fadeOutMs == 0 || currentFrame >= entry.fadeOutDoneFrame) {
                rendered = entry.targetValues;
                entry.phase = FixturePatternPhase::Done;
            } else {
                const double t = static_cast<double>(fadeElapsed) / static_cast<double>(entry.fadeOutMs);
                for (size_t i = 0; i < entry.channelSpan; ++i) {
                    const uint8_t a = (i < entry.startValues.size()) ? entry.startValues[i] : 0;
                    const uint8_t b = (i < entry.targetValues.size()) ? entry.targetValues[i] : 0;
                    rendered[i] = lerp(a, b, t);
                }
            }
            break;
        }
        case FixturePatternPhase::Done:
            // Should already be cleaned up; safety net.
            finished.push_back(entry.fixtureId);
            continue;
        }

        entry.lastRenderedValues = rendered;
        toEmit.push_back(&entry);

        if (entry.phase == FixturePatternPhase::Done) {
            finished.push_back(entry.fixtureId);
        }
    }

    // Count phases for telemetry. Per-fixture spans inside this loop would be one span per
    // fixture per 20ms — too expensive. Use summary counters on the parent tick span instead
    // (the BubbleUp-friendly pattern).
    size_t fadeInCount = 0, holdCount = 0, fadeOutCount = 0;
    for (auto *entry : toEmit) {
        switch (entry->phase) {
        case FixturePatternPhase::FadeIn:
            ++fadeInCount;
            break;
        case FixturePatternPhase::Hold:
            ++holdCount;
            break;
        case FixturePatternPhase::FadeOut:
            ++fadeOutCount;
            break;
        case FixturePatternPhase::Done:
            break;
        }
    }

    // Render any live-control entries. Live entries simply emit their current values
    // until the deadline frame, then emit a blackout and remove themselves. Mutually
    // exclusive with patterns per fixture (start() refuses when live exists; setLive()
    // cancels the active pattern), so we don't worry about ordering between the two.
    struct LiveEmit {
        universe_t universe;
        uint16_t channelOffset;
        std::vector<uint8_t> data;
        const std::string *triggerTraceId;
        const std::string *triggerSpanId;
        const fixtureId_t *fixtureId;
    };
    std::vector<LiveEmit> liveToEmit;
    std::vector<fixtureId_t> liveFinished;
    int64_t liveBlackoutCount = 0;

    for (auto &kv : live_) {
        auto &entry = kv.second;
        LiveEmit le;
        le.universe = entry.universe;
        le.channelOffset = entry.channelOffset;
        le.triggerTraceId = &entry.triggerTraceId;
        le.triggerSpanId = &entry.triggerSpanId;
        le.fixtureId = &entry.fixtureId;
        if (currentFrame >= entry.deadlineFrame) {
            // Deadline reached: emit one final blackout and queue for removal.
            le.data.assign(entry.channelSpan, 0);
            liveFinished.push_back(entry.fixtureId);
            ++liveBlackoutCount;
        } else {
            le.data = entry.values;
        }
        liveToEmit.push_back(std::move(le));
    }

    // Schedule DMX events. We keep the lock during scheduling so the entries don't get yanked
    // out from under us, but the eventLoop has its own internal synchronization.
    if (eventLoop) {
        for (auto *entry : toEmit) {
            auto dmxEvent = std::make_shared<DMXEvent>(currentFrame);
            dmxEvent->universe = entry->universe;
            dmxEvent->channelOffset = entry->channelOffset;
            dmxEvent->data = entry->lastRenderedValues;
            eventLoop->scheduleEvent(dmxEvent);
        }
        for (const auto &le : liveToEmit) {
            auto dmxEvent = std::make_shared<DMXEvent>(currentFrame);
            dmxEvent->universe = le.universe;
            dmxEvent->channelOffset = le.channelOffset;
            dmxEvent->data = le.data;
            eventLoop->scheduleEvent(dmxEvent);
        }
    } else {
        warn("FixturePatternRunner::tick: eventLoop unavailable, dropping {} pattern + {} live frames", toEmit.size(),
             liveToEmit.size());
        if (tickSpan) {
            tickSpan->setAttribute("error.type", "EventLoopUnavailable");
            tickSpan->setError("eventLoop unavailable, DMX frames dropped");
        }
    }

    if (tickSpan) {
        tickSpan->setAttribute("fixture.patterns.active_count", static_cast<int64_t>(toEmit.size()));
        tickSpan->setAttribute("fixture.patterns.finished_count", static_cast<int64_t>(finished.size()));
        tickSpan->setAttribute("fixture.patterns.fade_in_count", static_cast<int64_t>(fadeInCount));
        tickSpan->setAttribute("fixture.patterns.hold_count", static_cast<int64_t>(holdCount));
        tickSpan->setAttribute("fixture.patterns.fade_out_count", static_cast<int64_t>(fadeOutCount));
        tickSpan->setAttribute("fixture.live.active_count", static_cast<int64_t>(liveToEmit.size()));
        tickSpan->setAttribute("fixture.live.blackout_count", liveBlackoutCount);
        tickSpan->setAttribute("fixture.dmx_events.emitted",
                               static_cast<int64_t>(eventLoop ? (toEmit.size() + liveToEmit.size()) : 0));

        // Surface the trigger trace IDs of the first entry (pattern or live) that has one.
        // This gives a single Honeycomb-searchable link back to the originating REST/activity
        // span. (Multiple originating triggers per tick is rare; if it happens we lose
        // visibility of the second+ — fine for v1, the tick span is sampled anyway.)
        for (auto *entry : toEmit) {
            if (!entry->triggerTraceId.empty()) {
                tickSpan->setAttribute("trigger.trace_id", entry->triggerTraceId);
                tickSpan->setAttribute("trigger.span_id", entry->triggerSpanId);
                tickSpan->setAttribute("trigger.fixture.id", entry->fixtureId);
                goto trigger_attrs_done;
            }
        }
        for (const auto &le : liveToEmit) {
            if (!le.triggerTraceId->empty()) {
                tickSpan->setAttribute("trigger.trace_id", *le.triggerTraceId);
                tickSpan->setAttribute("trigger.span_id", *le.triggerSpanId);
                tickSpan->setAttribute("trigger.fixture.id", *le.fixtureId);
                break;
            }
        }
    trigger_attrs_done:;
    }

    // Clean up finished entries.
    for (const auto &id : finished) {
        active_.erase(id);
    }
    for (const auto &id : liveFinished) {
        live_.erase(id);
    }

    return !active_.empty() || !live_.empty();
}

} // namespace creatures
