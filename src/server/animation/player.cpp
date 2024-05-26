
#include <fmt/format.h>

#include "server/config.h"

#include "spdlog/spdlog.h"

#include "model/Animation.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "exception/exception.h"


#include "server/namespace-stuffs.h"
#include "util/cache.h"
#include "util/helpers.h"


namespace creatures {

    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    /**
     * Schedules an animation on a given creature
     *
     * @param startingFrame the frame number to start the animation on
     * @param animation the animation to play
     * @param universe the universe to play the animation on
     *
     * @return the frame number of last frame of the animation
     */
    Result<framenum_t> scheduleAnimation(framenum_t startingFrame, const creatures::Animation& animation, universe_t universe) {

       debug("scheduling animation {} ({}) on universe {} for frame {}",
             animation.metadata.title,
             animation.id,
             universe,
             startingFrame);


       /*
        * Let's validate that all of the creatures in the animation actually exist before we start to
        * do this. This is also making sure that the creature is in the cache, because if it's not the
        * database will hate us.
        */

       for( const auto& track : animation.tracks) {
           creatureId_t creatureId = track.creature_id;

           auto creatureResult = db->getCreature(creatureId);
           if(!creatureResult.isSuccess()) {
               auto error = creatureResult.getError().value();
               std::string errorMessage = fmt::format("Not able to play animation because creatureId {} doesn't exist",
                                                      creatureResult.getError()->getMessage());
               warn(errorMessage);
               return Result<framenum_t>{ServerError(ServerError::NotFound, errorMessage)};
           }
           debug("validated that creatureId {} exists! (It's {})", creatureId, creatureResult.getValue().value().name);

       }





       // Schedule this animation in the event loop
       trace("starting with frame {}", startingFrame);

       uint32_t msPerFrame = animation.metadata.milliseconds_per_frame;
       trace("playing at a speed of {}ms per frame", msPerFrame);

       // Look and see if there's an audio file to play with this animation
       if(!animation.metadata.sound_file.empty()) {

           // Set up the path to the sound file based on the startup config
           std::string soundFileName = config->getSoundFileLocation() + "/" + animation.metadata.sound_file;
           debug("using sound file name: {}", soundFileName);

           auto playSoundEvent = std::make_shared<MusicEvent>(startingFrame, soundFileName);
           eventLoop->scheduleEvent(playSoundEvent);
           trace("scheduled sound event for frame {}", startingFrame);
       }

       // Turn on the status light
       auto statusLightOn = std::make_shared<StatusLightEvent>(startingFrame, StatusLight::Animation, true);
       eventLoop->scheduleEvent(statusLightOn);


       // Keep track of the last frame we schedule. Bear in mind that all of the tracks may not be the same length
       framenum_t lastFrame;


       /*
        * Now look at each track and start scheduling events
        */
        for( const auto& track : animation.tracks) {
            creatureId_t creatureId = track.creature_id;

            auto creature = creatureCache->get(creatureId);
            if(!creature) {
            // Oh shit. This should never happen!
                auto errorMessage = fmt::format("Can't find creatureId {} in the creature cache after we verified that it's there earlier!",
                                  creatureId);
                critical(errorMessage);
                return Result<framenum_t>{ServerError(ServerError::InternalError, errorMessage)};
            }

            uint32_t numberOfFrames = 0;
            framenum_t currentFrame = startingFrame;

            // Go schedule the events for this track
            for(const auto& frame : track.frames) {

                auto thisFrame = std::make_shared<DMXEvent>(currentFrame);
                thisFrame->universe = universe;
                thisFrame->channelOffset = creature->channel_offset;

                auto frameData = decodeBase64(frame);

                for (uint8_t byte: frame) {

#if DEBUG_ANIMATION_PLAY
                    trace("byte {}: 0x{:02x}", i++, byte);
#endif
                    thisFrame->data.push_back(byte);
                }

                // Okay! We're ready to go. Schedule it.
                eventLoop->scheduleEvent(thisFrame);

                numberOfFrames += 1;
                currentFrame += (msPerFrame / EVENT_LOOP_PERIOD_MS);
            }

            std::string okayMessage = fmt::format("✅ Scheduled {} frames on creature {} at a pacing of {}ms per frame for frames {} to {}",
                                                  numberOfFrames, creature->name, msPerFrame, startingFrame, (currentFrame-msPerFrame));
            info(okayMessage);

            // Keep track of the last frame we scheduled
            if(lastFrame < (currentFrame-msPerFrame)) {
                lastFrame = currentFrame-msPerFrame;
                debug("set the last frame to {}", lastFrame);
            }

        }

       // Schedule turning off the light
       auto statusLightOff = std::make_shared<StatusLightEvent>(lastFrame, StatusLight::Animation, false);
       eventLoop->scheduleEvent(statusLightOff);

       // Leave a log message that we're done
       auto okayMessage = fmt::format("✅ Scheduled animation {} for frame {} to {}",
                                      animation.metadata.title, startingFrame, lastFrame);
       info(okayMessage);
       metrics->incrementAnimationsPlayed();

       // Let the caller know all is well
       return Result<framenum_t>{lastFrame};
    }

}
