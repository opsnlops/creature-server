#pragma once

#include "server/namespace-stuffs.h"
#include "util/helpers.h"

namespace creatures {

    uint64_t scheduleAnimation(uint64_t startingFrame, const CreatureId& creatureId, const AnimationId& animationId);

}