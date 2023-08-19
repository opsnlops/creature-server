
#include "spdlog/spdlog.h"

#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
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

    /**
     * Stop any playlists running on a creature
     *
     * @param context ServerContext from gPRC
     * @param creatureId the CreatureID to stop playlist playback on
     * @param response the response
     * @return a status for the end user
     */
    grpc::Status CreatureServerImpl::StopPlaylist(ServerContext *context,
                                                  const CreatureId *creatureId,
                                                  CreaturePlaylistResponse *response) {

        info("Stopping all playlists on creature {} due to a gRPC request", creatureIdToString(*creatureId));

        grpc::Status status;


        // Load the creature
        auto creature = std::make_shared<Creature>();
        try {
            db->getCreature(creatureId, creature.get());
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
            critical("Data format exception while loading a creature on playlist stop: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Data format exception while loading a creature on playlist stop");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty creatureID was passed in while attempting to play an animation");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }


        // Looks good!
        debug("the creature ID is valid! (it's {})", creature->name());


        // Remove the creature's key from the cache
        runningPlaylists->remove(creatureIdToString(*creatureId));
        trace("removed from map");


        std::string okayMessage = fmt::format("🎵 Stopped playlists on creature {}", creature->name());

        info(okayMessage);
        response->set_message(okayMessage);
        response->set_success(true);
        metrics->incrementPlaylistsStopped();

        status = grpc::Status(grpc::StatusCode::OK, okayMessage);
        return status;
    }

}