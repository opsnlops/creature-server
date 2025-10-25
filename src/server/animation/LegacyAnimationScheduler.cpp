//
// LegacyAnimationScheduler.cpp
// Reference implementation of bulk event scheduling
//

#include "LegacyAnimationScheduler.h"

#include <filesystem>
#include <fmt/format.h>

#include "server/config.h"

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "model/Animation.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

extern std::shared_ptr<Configuration> config;
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;

namespace {

std::filesystem::path resolveSoundFilePath(const std::string &soundFile) {
    if (soundFile.empty()) {
        return {};
    }
    std::filesystem::path path(soundFile);
    if (path.is_absolute()) {
        return path;
    }
    return std::filesystem::path(config->getSoundFileLocation()) / path;
}

} // namespace

/**
 * Schedules an animation on a given creature using the legacy bulk-scheduling approach
 *
 * This is the original implementation extracted as a reference. It schedules all
 * DMX frames and audio chunks upfront as individual events.
 *
 * @param startingFrame the frame number to start the animation on
 * @param animation the animation to play
 * @param universe the universe to play the animation on
 *
 * @return the frame number of last frame of the animation
 */
Result<framenum_t> LegacyAnimationScheduler::scheduleAnimation(framenum_t startingFrame,
                                                               const creatures::Animation &animation,
                                                               universe_t universe) {

    // Create a parent span for the entire animation scheduling operation
    auto scheduleSpan = observability->createOperationSpan("Animation.scheduleAnimation.legacy");
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.id", animation.id);
        scheduleSpan->setAttribute("animation.title", animation.metadata.title);
        scheduleSpan->setAttribute("animation.universe", static_cast<int64_t>(universe));
        scheduleSpan->setAttribute("animation.startFrame", static_cast<int64_t>(startingFrame));
        scheduleSpan->setAttribute("animation.tracksCount", static_cast<int64_t>(animation.tracks.size()));
        scheduleSpan->setAttribute("animation.msPerFrame",
                                   static_cast<int64_t>(animation.metadata.milliseconds_per_frame));
        scheduleSpan->setAttribute("scheduler.type", "legacy_bulk");
    }

    debug("scheduling animation {} ({}) on universe {} for frame {}", animation.metadata.title, animation.id, universe,
          startingFrame);

    /*
     * Let's validate that all of the creatures in the animation actually exist before we start to
     * do this. This is also making sure that the creature is in the cache, because if it's not the
     * database will hate us.
     */

    for (const auto &track : animation.tracks) {
        creatureId_t creatureId = track.creature_id;

        // Create a child span for this creature validation
        auto validationSpan =
            observability->createChildOperationSpan("scheduleAnimation.validateCreature", scheduleSpan);
        if (validationSpan) {
            validationSpan->setAttribute("creature.id", creatureId);
            validationSpan->setAttribute("animation.id", animation.id);
        }

        auto creatureResult = db->getCreature(creatureId, validationSpan);
        if (!creatureResult.isSuccess()) {
            auto error = creatureResult.getError().value();
            std::string errorMessage = fmt::format("Not able to play animation because creatureId {} doesn't exist",
                                                   creatureResult.getError()->getMessage());
            warn(errorMessage);
            if (validationSpan) {
                validationSpan->setError(errorMessage);
            }
            if (scheduleSpan) {
                scheduleSpan->setError(errorMessage);
            }
            return Result<framenum_t>{ServerError(ServerError::NotFound, errorMessage)};
        }

        if (validationSpan) {
            validationSpan->setAttribute("creature.name", creatureResult.getValue().value().name);
            validationSpan->setSuccess();
        }
        debug("validated that creatureId {} exists! (It's {})", creatureId, creatureResult.getValue().value().name);
    }

    // Schedule this animation in the event loop
    trace("starting with frame {}", startingFrame);

    uint32_t msPerFrame = animation.metadata.milliseconds_per_frame;
    trace("playing at a speed of {}ms per frame", msPerFrame);

    // Look and see if there's an audio file to play with this animation
    if (!animation.metadata.sound_file.empty()) {

        auto soundFilePath = resolveSoundFilePath(animation.metadata.sound_file);
        debug("using sound file name: {}", soundFilePath.string());

        auto playSoundEvent = std::make_shared<MusicEvent>(startingFrame, soundFilePath.string());
        eventLoop->scheduleEvent(playSoundEvent);
        trace("scheduled sound event for frame {}", startingFrame);
    }

    // Turn on the status light
    auto statusLightOn = std::make_shared<StatusLightEvent>(startingFrame, StatusLight::Animation, true);
    eventLoop->scheduleEvent(statusLightOn);

    // Keep track of the last frame we schedule. Bear in mind that all of the tracks may not be the same length
    framenum_t lastFrame = startingFrame;

    /*
     * Now look at each track and start scheduling events
     */
    for (const auto &track : animation.tracks) {
        creatureId_t creatureId = track.creature_id;

        // Get the creature for this track - check cache first, then DB
        std::shared_ptr<Creature> creature;
        if (creatureCache->contains(creatureId)) {
            creature = creatureCache->get(creatureId);
        } else {
            // Not in cache - fetch from database and cache it
            debug("Creature {} not in cache, fetching from database", creatureId);
            auto creatureResult = db->getCreature(creatureId, nullptr);
            if (!creatureResult.isSuccess()) {
                std::string errorMsg =
                    fmt::format("Creature {} not found in database during legacy playback", creatureId);
                error(errorMsg);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMsg)};
            }
            creature = std::make_shared<Creature>(creatureResult.getValue().value());
            creatureCache->put(creatureId, creature);
            debug("Cached creature {} for playback", creatureId);
        }

        uint32_t numberOfFrames = 0;
        framenum_t currentFrame = startingFrame;

        // Go schedule the events for this track
        for (const auto &frame : track.frames) {

            auto thisFrame = std::make_shared<DMXEvent>(currentFrame);
            thisFrame->universe = universe;
            thisFrame->channelOffset = creature->channel_offset;

            auto frameData = decodeBase64(frame);
            thisFrame->data.reserve(frameData.size());

#if DEBUG_ANIMATION_PLAY
            debug("Player Frame: {}", vectorToHexString(frameData));
#endif
            for (uint8_t byte : frameData) {
                thisFrame->data.push_back(byte);
            }

            // Okay! We're ready to go. Schedule it.
            eventLoop->scheduleEvent(thisFrame);

            numberOfFrames += 1;
            currentFrame += (msPerFrame / EVENT_LOOP_PERIOD_MS);
        }

        std::string okayMessage =
            fmt::format("✅ Scheduled {} frames on creature {} at a pacing of {}ms per frame for frames {} to {}",
                        numberOfFrames, creature->name, msPerFrame, startingFrame, (currentFrame - msPerFrame));
        info(okayMessage);

        // Keep track of the last frame we scheduled
        if (lastFrame < (currentFrame - msPerFrame)) {
            lastFrame = currentFrame - msPerFrame;
            debug("set the last frame to {}", lastFrame);
        }
    }

    // Schedule turning off the light
    auto statusLightOff = std::make_shared<StatusLightEvent>(lastFrame, StatusLight::Animation, false);
    eventLoop->scheduleEvent(statusLightOff);

    // Leave a log message that we're done
    auto okayMessage =
        fmt::format("✅ Scheduled animation {} for frame {} to {}", animation.metadata.title, startingFrame, lastFrame);
    info(okayMessage);
    metrics->incrementAnimationsPlayed();

    // Mark the overall scheduling as successful
    if (scheduleSpan) {
        scheduleSpan->setAttribute("animation.lastFrame", static_cast<int64_t>(lastFrame));
        scheduleSpan->setAttribute("animation.totalFramesScheduled", static_cast<int64_t>(lastFrame - startingFrame));
        scheduleSpan->setSuccess();
    }

    // Let the caller know all is well
    return Result<framenum_t>{lastFrame};
}

} // namespace creatures
