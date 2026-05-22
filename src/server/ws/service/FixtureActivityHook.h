
#pragma once

#include <functional>

#include "server/namespace-stuffs.h"
#include "server/runtime/Activity.h"

namespace creatures {

/**
 * Hook type used by CreatureService::setActivityState to notify the fixture binding
 * dispatcher of activity transitions.
 *
 * Why a hook and not a direct method call? Decouples CreatureService from the fixture
 * subsystem at link time. The main binary installs the hook in main.cpp; the test binary
 * leaves it default-constructed (a no-op).
 */
using FixtureActivityHook =
    std::function<void(const creatureId_t &creatureId, runtime::ActivityReason reason, runtime::ActivityState state)>;

} // namespace creatures
