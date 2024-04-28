#pragma once

#include "server/namespace-stuffs.h"
#include "util/helpers.h"

namespace creatures {

    /**
     * Schedules an animation to be played on a given universe
     *
     * Playing an animation can be fairly complex. This function will
     * load the animation from the database, look up the creatures that
     * are suppose to do a thing, and then schedule the animation in the
     * event loop.
     *
     * @param startingFrame
     * @param animationId
     * @param universe
     * @return the last frame number of the animation
     */
    uint64_t scheduleAnimation(uint64_t startingFrame, const std::string& animationId, universe_t universe);

}