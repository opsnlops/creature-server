

#include <vector>
#include <random>
#include <algorithm>

#include "spdlog/spdlog.h"

#include "server/animation/player.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

#include "exception/exception.h"
#include "util/cache.h"
#include "util/helpers.h"

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObjectCache<std::string, PlaylistIdentifier>> runningPlaylists;

    PlaylistEvent::PlaylistEvent(uint64_t frameNumber, std::string creatureIdString)
            : EventBase(frameNumber), creatureIdString(std::move(creatureIdString)) {}

    void PlaylistEvent::executeImpl() {


        debug("hello from a playlist event for creature {}", creatureIdString);
        metrics->incrementPlaylistsEventsProcessed();


        std::shared_ptr<PlaylistIdentifier> activePlaylistId;

        // Load the playlist
        try {
            activePlaylistId = runningPlaylists->get(creatureIdString);
            debug("found a playlist for creature {} in the cache", creatureIdString);
        }
            catch (std::out_of_range& e) {
            info("No active playlist for creature {}. Assuming we've been stopped.", creatureIdString);
            return;
        }


        Playlist playlist = Playlist();
        try {
            db->getPlaylist(activePlaylistId.get(), &playlist);
        }
        catch(NotFoundException &e) {
            warn("Playlist ID {} not found while in a playlist event: {}", activePlaylistId->_id(), e.what());
            return;
        }
        catch(InvalidArgumentException &e) {
            critical("Invalid argument while loading a playlist on a playlist event: {}", e.what());
            return;
        }
        catch( ... )
        {
            error("Unknown exception while loading a playlist on a playlist event");
            return;
        }
        debug("playlist found. name: {}", playlist.name());

        /*
         * Brute force way to determine which animation to play
         *
         * This isn't the best way to do this, but the amount of memory needed is pretty small, really, and we've
         * got the RAM and CPU to burn.
         *
         * Create a list of every possible animation in the playlist. Add it the number of times that it's weighted,
         * and then pick a random one from the list of weighted animations.
         */

        std::vector<AnimationId> choices;
        for(const auto& playlistId : playlist.items()) {
            for(int i = 0; i < playlistId.weight(); i++) {
                choices.push_back(playlistId.animationid());
            }
            debug("added an animation to the list. {} now possible", choices.size());
        }

        // Pick a random animation the C++20 way
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, choices.size() - 1);
        size_t theChosenOne = dis(gen);

        auto chosenAnimation = choices[theChosenOne];
        debug("...and the chosen one is {}", animationIdToString(chosenAnimation));

        CreatureId creatureId = stringToCreatureId(creatureIdString);

        // Schedule this animation
        uint64_t lastFrame = scheduleAnimation(eventLoop->getNextFrameNumber(), creatureId, chosenAnimation);
        debug("scheduled animation {}. Last frame: {}", animationIdToString(chosenAnimation), lastFrame);


        // Add another one of us to go again later
        auto nextEvent = std::make_shared<PlaylistEvent>(lastFrame+1, creatureIdString);
        eventLoop->scheduleEvent(nextEvent);
        debug("scheduled next event for frame {}. later!", lastFrame+1);
    }

}