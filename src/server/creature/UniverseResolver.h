
#pragma once

#include <vector>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * What playback should target for a set of creatures, and who wasn't there.
 */
struct PlaybackUniverse {
    universe_t universe{};

    /**
     * Participants with no universe registration — almost always a controller
     * that is powered off.
     *
     * They are NOT dropped from the scene. A creature's audio is a channel in
     * the 17-channel room mix, so it plays into the room whether or not the
     * body is present, and its voice and animation come from the database
     * rather than from its controller. Its servo track simply goes nowhere.
     * Dropping it would leave the room hearing a hole where its line belongs.
     *
     * Carried out purely so callers can record who was absent (issue #233).
     */
    std::vector<creatureId_t> unregistered;
};

/**
 * Resolve the single universe a multi-creature animation should play into.
 *
 * Playback is scheduled per universe, so every trigger that plays a
 * multi-creature animation (the dialog render's autoplay, the dialog stream,
 * POST /api/v1/animation/ad-hoc/play) needs the same rule. This is the one
 * shared implementation — don't re-implement it inline.
 *
 * The universe is a property of which ROOM the show is playing into, not of
 * each individual creature, so it is resolved from the participants that are
 * registered. A participant that isn't registered is still a full member of
 * the scene; see PlaybackUniverse::unregistered.
 *
 * This deliberately degrades rather than refusing: before issue #233 one
 * powered-off creature refused an entire dialog, so a Creature World response
 * built its animation and then nothing talked at all.
 *
 * Errors:
 *  - Conflict: NO participant is registered, so there is nothing to work from
 *  - InvalidData: the registered participants span more than one universe
 *  - InvalidData: no creatures were given
 *  - InternalError: the creature/universe map isn't available
 */
Result<PlaybackUniverse> resolvePlaybackUniverse(const std::vector<creatureId_t> &creatureIds);

} // namespace creatures
