#pragma once

#include "server/namespace-stuffs.h"
#include "util/helpers.h"
#include "util/Result.h"

namespace creatures {

    /**
     * Schedules an animation to be played on a given universe
     *
     * Playing an animation can be fairly complex. This function will
     * load the animation from the database, look up the creatures that
     * are suppose to do a thing, and then schedule the animation in the
     * event loop.
     *
     * @param startingFrame the frame number to start the animation on
     * @param animation the animation to play
     * @param universe the universe to play the animation on
     * @return the last frame number of the animation
     */
    Result<framenum_t> scheduleAnimation(framenum_t startingFrame, const creatures::Animation& animation, universe_t universe);

}