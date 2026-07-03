
#pragma once

#include <vector>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * Resolve the single universe shared by every creature in the set.
 *
 * Playback is scheduled per-universe, so every trigger that plays a multi-creature
 * animation (the dialog render's autoplay, POST /api/v1/animation/ad-hoc/play) needs the
 * same rule: each creature must be registered with a universe, and they must all agree.
 * This is the one shared implementation of that rule — don't re-implement it inline.
 *
 * Errors:
 *  - Conflict: a creature isn't registered with a universe (is its controller online?)
 *  - InvalidData: the creatures span more than one universe
 *  - InternalError: the creature/universe map isn't available
 */
Result<universe_t> resolveCommonUniverse(const std::vector<creatureId_t> &creatureIds);

} // namespace creatures
