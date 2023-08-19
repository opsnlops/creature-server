
#include "server/config.h"

#include "spdlog/spdlog.h"

#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"

#include "exception/exception.h"
#include "util/cache.h"
#include "util/helpers.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObjectCache<std::string, PlaylistIdentifier>> runningPlaylists;

    grpc::Status CreatureServerImpl::StartPlaylist(ServerContext *context,
                                                   const CreaturePlaylistRequest *request,
                                                   CreaturePlaylistResponse *response) {

        info("Starting a playlist from a gRPC request");

        grpc::Status status;

        auto creatureId = request->creatureid();
        auto playlistId = request->playlistid();

        // Load the playlist
        auto playlist = std::make_shared<Playlist>();
        try {
            db->getPlaylist(&playlistId, playlist.get());
            debug("loaded playlist {}", playlist->name());
        }
        catch (const NotFoundException &e) {
            info("playlist not found");
            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
                                  e.what(),
                                  fmt::format("🚫 Playlist id not found"));
            return status;
        }
        catch (const DataFormatException &e) {
            critical("Data format exception while loading a playlist on animation playback: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Data format exception while loading a playlist on animation playback");
            return status;
        }

        // Load the creature
        auto creature = std::make_shared<Creature>();
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


        // Let the user know things are good
        debug("both the creature ID and the playlist ID are valid");




        // Set the playlist in the cache
        runningPlaylists->put(creatureIdToString(creatureId), playlistId);

        auto playEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), creatureIdToString(creatureId));
        eventLoop->scheduleEvent(playEvent);

        std::string okayMessage = fmt::format("🎵 Started playing playlist {} on creature {}", playlist->name(), creature->name());

        info(okayMessage);
        response->set_message(okayMessage);
        response->set_success(true);
        metrics->incrementPlaylistsStarted();

        status = grpc::Status(grpc::StatusCode::OK,
                              okayMessage);
        return status;
    }

}