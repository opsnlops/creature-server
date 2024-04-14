
#include <fmt/format.h>

#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "exception/exception.h"


#include "server/namespace-stuffs.h"
#include "util/helpers.h"


namespace creatures {

    extern std::shared_ptr<Configuration> config;
    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    /**
     * Schedules an animation on a given creature
     *
     * @param animationId the AnimationId of the animation
     * @param universe the universe to play the animation on
     *
     * @return the frame number of last frame of the animation
     */
    uint64_t scheduleAnimation(uint64_t startingFrame, const AnimationId& animationId, universe_t universe) {

        debug("scheduling animation {} on universe {}", animationIdToString(animationId), universe);


        // Load the animation
        auto animation = std::make_unique<Animation>();
        try {
            db->getAnimation(&animationId, animation.get());
            debug("loaded animation {}", animation->metadata().title());
        }
        catch(const NotFoundException &e) {
            warn("animation ({}) not found while attempting to schedule an animation", animationIdToString(animationId));
            std::throw_with_nested(e);
        }
        catch(const DataFormatException &e) {
            error("data format error while loading an animation ({}) for playback", animationIdToString(animationId));
            std::throw_with_nested(e);
        }
        catch (...) {
            warn("an unknown exception was thrown while attempting to load a animation to schedule an animation");
            throw;
        }

        info("Attempting to play animation {} ({})", animation->metadata().title(), animationIdToString(animationId));



        for(const auto& a : animation->frames()) {
            CreatureId creatureId = stringToCreatureId(a.creature_id());

            auto creature = std::make_unique<server::Creature>();
            db->getCreature(&creatureId, creature.get());

            info("One of the creatures is {}", creature->name());
        }

        return 0;

#warning remember that this is a stub and needs to be implemented
        /*

        // Load the creature
        auto creature = std::make_unique<Creature>();
        try {
            db->getCreature(&creatureId, creature.get());
            debug("loaded creature {}", creature->name());
        }
        catch (const creatures::NotFoundException &e) {
            warn("Creature ({}) not found on a request to schedule an animation", creatureIdToString(creatureId));
            std::throw_with_nested(e);
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while loading a creature on animation playback: {}", e.what());
            std::throw_with_nested(e);
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty creatureID was passed in while attempting to play an animation");
            std::throw_with_nested(e);
        }
        catch (...) {
            warn("an unknown exception was thrown while attempting to load a creature to schedule an animation");
            throw;
        }




        // Schedule this animation in the event loop
        trace("starting with frame {}", startingFrame);

        uint32_t msPerFrame = animation->metadata().milliseconds_per_frame();
        trace("playing at a speed of {}ms per frame", msPerFrame);

        // Look and see if there's an audio file to play with this animation
        if(!animation->metadata().sound_file().empty()) {

            // Set up the path to the sound file based on the startup config
            std::string soundFileName = config->getSoundFileLocation() + "/" + animation->metadata().sound_file();
            debug("using sound file name: {}", soundFileName);

            auto playSoundEvent = std::make_shared<MusicEvent>(startingFrame, soundFileName);
            eventLoop->scheduleEvent(playSoundEvent);
            trace("scheduled sound event for frame {}", startingFrame);
        }

        // Turn on the status light
        auto statusLightOn = std::make_shared<StatusLightEvent>(startingFrame, StatusLight::Animation, true);
        eventLoop->scheduleEvent(statusLightOn);

        uint32_t numberOfFrames = 0;
        uint64_t currentFrame = startingFrame;
        for(const auto& frame : animation->frames()) {

            auto thisFrame = std::make_shared<DMXEvent>(currentFrame);

            thisFrame->channelOffset = creature->channel_offset();
            thisFrame->universe = creature->universe();
            thisFrame->numMotors = creature->number_of_motors();

            // Get the frame field from the protobuf message
            const auto& frame_bytes = frame.bytes(0);
            std::vector<uint8_t> data(frame_bytes.begin(), frame_bytes.end());
            thisFrame->data = data;

            eventLoop->scheduleEvent(thisFrame);

            numberOfFrames++;

#if DEBUG_ANIMATION_PLAY
            trace("scheduled animation frame {} for event loop frame {}",
                  numberOfFrames, currentFrame);

            trace("Frame length: {}", frame.ByteSizeLong());
            std::ostringstream oss;
            for (const auto& value : data) {
                oss << static_cast<int>(value) << " ";
            }
            trace("Frame data on send: {}", oss.str());
#endif

            currentFrame += (msPerFrame / EVENT_LOOP_PERIOD_MS);
        }

        // Schedule turning off the light
        auto statusLightOff = std::make_shared<StatusLightEvent>((currentFrame-msPerFrame), StatusLight::Animation, false);
        eventLoop->scheduleEvent(statusLightOff);

        std::string okayMessage = fmt::format("✅ Scheduled {} frames on creature {} at a pacing of {}ms per frame for frames {} to {}",
                                              numberOfFrames, creature->name(), msPerFrame, startingFrame, (currentFrame-msPerFrame));
        info(okayMessage);
        metrics->incrementAnimationsPlayed();

        // Let the caller know all is well
        return currentFrame-msPerFrame;
         */
    }

}
