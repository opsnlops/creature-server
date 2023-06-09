
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "exception/exception.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;

    grpc::Status CreatureServerImpl::PlayAnimation(grpc::ServerContext *context, const PlayAnimationRequest *request,
                                                   PlayAnimationResponse *response) {

        info("Playing an animation from a gRPC request");

        grpc::Status status;

        // Load the creature
        auto creature = std::make_shared<Creature>();
        CreatureId creatureId = request->creatureid();

        try {
            db->getCreature(&creatureId, creature.get());
            debug("loaded creature {}", creature->name());
        }
        catch (const creatures::NotFoundException &e) {
            info("creature not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Creature id not found"));
            return status;
        }
        catch (const creatures::DataFormatException &e) {
            critical("Data format exception while loading a creature on animation playback: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Data format exception while loading a creature on animation playback");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty creatureID was passed in while attempting to play an animation");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }


        // Load the animation
        auto animation = std::make_shared<Animation>();
        AnimationId animationId = request->animationid();

        try {
            db->getAnimation(&animationId, animation.get());
            debug("loaded animation {}", animation->metadata().title());
        }
        catch(const NotFoundException &e) {
            info("animation not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  "🚫 Animation ID not found");
            return status;
        }
        catch(const DataFormatException &e) {
            error("data format error while loading an animation for playback");
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Database formatting error?");
            return status;
        }


#ifdef DISABLE_PLAY_SAFETY_CHECKS
#warning Animation playing saftey checks off!
#elif
        // Make sure this animation is for this type of creature
        if(animation->metadata().creature_type() != creature->type()) {
            warn("attempted to play an animation of type {} on a creature of type {}",
                 animation->metadata().creature_type(),
                 creature->type());
            status = grpc::Status(grpc::StatusCode::ABORTED,
                                  "Creature and Animation are of different types",
                                  fmt::format("Creature is of type {}, and Animation is of type {}",
                                              creature->type(),
                                              animation->metadata().creature_type()
                                  ));
            return status;
        }
        debug("passed check of animation type and creature type");

        // TODO: Make sure the number of motors is the same, maybe?
#endif

        // Schedule this animation in the event loop
        uint64_t startingFrame = eventLoop->getNextFrameNumber() + 500; // Wait 500ms before starting
        trace("starting with frame {}", startingFrame);

        uint32_t msPerFrame = animation->metadata().milliseconds_per_frame();
        trace("playing at a speed of {}ms oer frame", msPerFrame);

        // Look and see if there's an audio file to play with this animation
        if(!animation->metadata().sound_file().empty()) {

            // Set up the path to the sound file based on the startup config
            std::string soundFileName = MusicEvent::getSoundFileLocation() + "/" + animation->metadata().sound_file();
            debug("using sound file name: {}", soundFileName);

            auto playSoundEvent = std::make_shared<MusicEvent>(startingFrame, soundFileName);
            eventLoop->scheduleEvent(playSoundEvent);
            trace("scheduled sound event for frame {}", startingFrame);
        }

        uint32_t numberOfFrames = 0;
        uint64_t currentFrame = startingFrame;
        for(const auto& frame : animation->frames()) {

            auto thisFrame = std::make_shared<DMXEvent>(currentFrame);

            thisFrame->clientIP = creature->sacn_ip();
            thisFrame->dmxOffset = creature->dmx_base();
            thisFrame->dmxUniverse = creature->universe();
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

        std::string okayMessage = fmt::format("✅ Scheduled {} frames on creature {} at a pacing of {}ms per frame for frames {} to {}",
        numberOfFrames, creature->name(), msPerFrame, startingFrame, (currentFrame-msPerFrame));

        info(okayMessage);
        *response->mutable_status() = okayMessage;
        metrics->incrementAnimationsPlayed();

        status = grpc::Status(grpc::StatusCode::OK,
                              okayMessage);
        return status;
    }

}