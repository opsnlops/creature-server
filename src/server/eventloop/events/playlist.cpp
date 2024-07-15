

#include <vector>
#include <random>
#include <algorithm>

#include "spdlog/spdlog.h"

#include "model/PlaylistStatus.h"
#include "server/animation/player.h"
#include "server/database.h"
#include "server/eventloop/event.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "server/namespace-stuffs.h"

#include "util/cache.h"
#include "util/websocketUtils.h"

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;

    PlaylistEvent::PlaylistEvent(framenum_t frameNumber, universe_t universe)
            : EventBase(frameNumber), activeUniverse(universe) {}

    void PlaylistEvent::executeImpl() {

        debug("hello from a playlist event for universe {}", activeUniverse);
        metrics->incrementPlaylistsEventsProcessed();


        // Is there a playlist for this universe?
        if(!runningPlaylists->contains(activeUniverse)) {
            info("No active playlist for universe {}. Assuming we've been stopped.", activeUniverse);
            runningPlaylists->remove(activeUniverse);
            sendEmptyPlaylistUpdate(activeUniverse);
            return;
        }


        // Go fetch the active playlist
        auto activePlaylistStatus = runningPlaylists->get(activeUniverse);
        debug("the active playlistStatus out of the cache is {}", activePlaylistStatus->playlist);


        // Go look this one up
        auto playListResult = db->getPlaylist(activePlaylistStatus->playlist);
        if(!playListResult.isSuccess()) {
            warn("Playlist ID {} not found while in a playlist event. halting playback.", activePlaylistStatus->playlist);
            runningPlaylists->remove(activeUniverse);
            sendEmptyPlaylistUpdate(activeUniverse);
            return;
        }
        auto playlist = playListResult.getValue().value();
        debug("playlist found. name: {}", playlist.name);


        /*
         * Brute force way to determine which animation to play
         *
         * This isn't the best way to do this, but the amount of memory needed is pretty small, really, and we've
         * got the RAM and CPU to burn.
         *
         * Create a list of every possible animation in the playlist. Add it the number of times that it's weighted,
         * and then pick a random one from the list of weighted animations.
         */

        std::vector<std::string> choices;
        for(const auto& playlistItem : playlist.items) {
            for(uint32_t i = 0; i < playlistItem.weight; i++) {
                choices.push_back(playlistItem.animation_id);
            }
            debug("added an animation to the list. {} now possible", choices.size());
        }

        // Pick a random animation the C++20 way
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<size_t> dis(0, choices.size() - 1);
        size_t theChosenOne = dis(gen);

        auto chosenAnimation = choices[theChosenOne];
        debug("...and the chosen one is {}", chosenAnimation);

        // Go get this animation
        Result<Animation> animationResult = db->getAnimation(chosenAnimation);
        if(!animationResult.isSuccess()) {
            warn("Animation ID {} not found while in a playlist event. halting playback.", chosenAnimation);
            runningPlaylists->remove(activeUniverse);
            sendEmptyPlaylistUpdate(activeUniverse);
            return;
        }
        auto animation = animationResult.getValue().value();

        // Schedule this animation
        auto scheduleResult = scheduleAnimation(eventLoop->getNextFrameNumber(), animation, activeUniverse);
        if(!scheduleResult.isSuccess()) {
            warn("Unable to schedule animation: {}. Halting playback.", scheduleResult.getError().value().getMessage());
            runningPlaylists->remove(activeUniverse);
            sendEmptyPlaylistUpdate(activeUniverse);
            return;
        }
        auto lastFrame = scheduleResult.getValue().value();

        debug("scheduled animation {} on universe {}. Last frame: {}",
              animation.metadata.title,
              activeUniverse,
              lastFrame);

        // Add another one of us to go again later
        auto nextEvent = std::make_shared<PlaylistEvent>(lastFrame+1, activeUniverse);
        eventLoop->scheduleEvent(nextEvent);


        // Update the cache with the animation we're currently playing
        activePlaylistStatus->current_animation = chosenAnimation;
        runningPlaylists->put(activeUniverse, activePlaylistStatus);

        sendPlaylistUpdate(*activePlaylistStatus);

        debug("scheduled next event for frame {}. later!", lastFrame+1);
    }

    /**
     * Send a playlist update to all clients
     *
     * This can be used to let the clients know that the playlist has changed, and what the current animation is.
     *
     * @param playlistStatus the PlaylistStatus to send
     */
    void PlaylistEvent::sendPlaylistUpdate(const PlaylistStatus &playlistStatus) {

        auto result = broadcastPlaylistStatusToAllClients(playlistStatus);
        if(!result.isSuccess()) {
            warn("Unable to broadcast playlist status to all clients: {}", result.getError().value().getMessage());
            // Just log. It's not a critical error.
        }
    }

    void PlaylistEvent::sendEmptyPlaylistUpdate(universe_t universe) {
        PlaylistStatus emptyStatus{};
        emptyStatus.universe = universe;
        emptyStatus.playlist = "";
        emptyStatus.playing = false;
        emptyStatus.current_animation = "";
        sendPlaylistUpdate(emptyStatus);
    }


}