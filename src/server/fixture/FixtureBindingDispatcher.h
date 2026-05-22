
#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include "server/namespace-stuffs.h"
#include "server/runtime/Activity.h"

namespace creatures {

class OperationSpan;

/**
 * Watches creature activity transitions and arms/disarms fixture patterns to match.
 *
 * Called from `CreatureService::setActivityState` once per affected creature. Iterates
 * `fixtureCache`, finds bindings whose `creature_id` matches, and for each:
 *   - if the new (reason, state) matches the binding's filters → start the pattern
 *   - if the previous (reason, state) matched but the new one doesn't → stop the pattern
 *
 * Edge-triggered: a per-creature last-seen cache suppresses no-op transitions (e.g. an idle
 * keepalive that re-asserts the same state).
 */
class FixtureBindingDispatcher {
  public:
    FixtureBindingDispatcher() = default;

    void onCreatureActivity(const creatureId_t &creatureId, runtime::ActivityReason reason,
                            runtime::ActivityState state, std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Does this (reason, state) pair satisfy a binding's optional filters? Pure function;
     * defined inline so tests can use it without linking the rest of the fixture subsystem.
     *
     * `nullopt` filters act as wildcards.
     */
    static bool matches(const std::optional<std::string> &filterReason, const std::optional<std::string> &filterState,
                        runtime::ActivityReason reason, runtime::ActivityState state) {
        if (filterReason.has_value() && *filterReason != runtime::toString(reason))
            return false;
        if (filterState.has_value() && *filterState != runtime::toString(state))
            return false;
        return true;
    }

  private:
    struct LastSeen {
        runtime::ActivityReason reason;
        runtime::ActivityState state;
    };

    std::mutex mutex_;
    std::unordered_map<creatureId_t, LastSeen> lastSeen_;

    // Per-creature mutex map. We hold one of these across the whole onCreatureActivity
    // body so concurrent transitions for the same creature can't race on the runner.
    // Different creatures still process in parallel.
    std::mutex creatureMutexMapMutex_;
    std::unordered_map<creatureId_t, std::shared_ptr<std::mutex>> creatureMutexes_;
};

} // namespace creatures
