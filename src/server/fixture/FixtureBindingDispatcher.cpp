
#include "FixtureBindingDispatcher.h"

#include <memory>

#include <spdlog/spdlog.h>

#include "FixturePatternRunner.h"
#include "FixturePatternTickEvent.h"
#include "model/DmxFixture.h"
#include "server/eventloop/eventloop.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {

extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;
extern std::shared_ptr<ObjectCache<fixtureId_t, universe_t>> fixtureUniverseMap;
extern std::shared_ptr<FixturePatternRunner> fixturePatternRunner;

namespace {

void armTickIfNeeded() {
    if (!fixturePatternRunner || !eventLoop)
        return;
    if (!fixturePatternRunner->tryArm())
        return; // Already armed — an existing tick will pick this up on its next pass.

    const auto next = eventLoop->getNextFrameNumber();
    auto tick = std::make_shared<FixturePatternTickEvent>(next);
    eventLoop->scheduleEvent(tick);
}

} // namespace

void FixtureBindingDispatcher::onCreatureActivity(const creatureId_t &creatureId, runtime::ActivityReason reason,
                                                  runtime::ActivityState state,
                                                  std::shared_ptr<OperationSpan> parentSpan) {

    if (creatureId.empty() || !fixtureCache || !fixturePatternRunner)
        return;

    auto span = observability
                    ? observability->createChildOperationSpan("FixtureBindingDispatcher.onCreatureActivity", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("creature.id", creatureId);
        span->setAttribute("activity.reason", runtime::toString(reason));
        span->setAttribute("activity.state", runtime::toString(state));
    }

    // Serialize concurrent transitions for the *same* creature. Different creatures still
    // process in parallel. Previously the edge check + lastSeen update happened under the
    // global mutex but the fixture scan ran outside it — so two transitions for the same
    // creature could both pass the edge check, both stomp `lastSeen_`, and then race
    // start()/stop() calls to the runner. Holding a per-creature mutex across the whole
    // body collapses that window (security review M2).
    std::shared_ptr<std::mutex> creatureMutex;
    {
        std::lock_guard<std::mutex> mapLock(creatureMutexMapMutex_);
        auto &slot = creatureMutexes_[creatureId];
        if (!slot)
            slot = std::make_shared<std::mutex>();
        creatureMutex = slot;
    }
    std::lock_guard<std::mutex> creatureLock(*creatureMutex);

    // Edge-triggered: bail if nothing changed.
    std::optional<LastSeen> prev;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = lastSeen_.find(creatureId);
        if (it != lastSeen_.end()) {
            prev = it->second;
            if (it->second.reason == reason && it->second.state == state) {
                return;
            }
        }
        lastSeen_[creatureId] = LastSeen{reason, state};
    }

    const auto fixtureIds = fixtureCache->getAllKeys();
    if (fixtureIds.empty())
        return;

    const framenum_t currentFrame = eventLoop ? eventLoop->getNextFrameNumber() : 0;
    bool startedAny = false;

    for (const auto &fid : fixtureIds) {
        std::shared_ptr<DmxFixture> fixture;
        try {
            fixture = fixtureCache->get(fid);
        } catch (const std::out_of_range &) {
            continue;
        }
        if (!fixture)
            continue;

        for (const auto &binding : fixture->bindings) {
            if (binding.creature_id != creatureId)
                continue;

            const bool nowMatches = matches(binding.on_reason, binding.on_state, reason, state);
            const bool prevMatched =
                prev.has_value() && matches(binding.on_reason, binding.on_state, prev->reason, prev->state);

            if (nowMatches && !prevMatched) {
                const FixturePattern *pattern = fixture->findPatternById(binding.pattern_id);
                if (!pattern) {
                    warn("Binding on fixture {} references unknown pattern {}", fixture->id, binding.pattern_id);
                    continue;
                }
                const auto universePtr = fixtureUniverseMap ? fixtureUniverseMap->tryGet(fid) : nullptr;
                if (!universePtr) {
                    debug("Skipping pattern {} on fixture {} — no assigned_universe", binding.pattern_id, fid);
                    continue;
                }
                const universe_t universe = *universePtr;
                if (fixturePatternRunner->start(*fixture, *pattern, universe, creatureId, currentFrame)) {
                    startedAny = true;
                }
            } else if (!nowMatches && prevMatched) {
                fixturePatternRunner->stop(fid, currentFrame);
                startedAny = true; // need a tick to render the fade-out
            }
        }
    }

    if (startedAny) {
        armTickIfNeeded();
    }

    if (span)
        span->setSuccess();
}

} // namespace creatures
