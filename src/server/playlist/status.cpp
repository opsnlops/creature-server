
#include "spdlog/spdlog.h"

#include "server/creature-server.h"
#include "server/database.h"
#include "server/metrics/counters.h"

#include "exception/exception.h"
#include "util/cache.h"
#include "util/helpers.h"


#include "server/namespace-stuffs.h"


namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<SystemCounters> metrics;
    extern std::shared_ptr<ObjectCache<std::string, PlaylistIdentifier>> runningPlaylists;


    Status CreatureServerImpl::GetPlaylistStatus(grpc::ServerContext *context,
                                                 const CreatureId *creatureId,
                                                 CreaturePlaylistStatus *response) {

        debug("incoming playlist status request for {}", creatureIdToString(*creatureId));
        metrics->incrementPlaylistStatusRequests();

        grpc::Status status;

        // Used as a buffer
        PlaylistIdentifier playlistId = PlaylistIdentifier();

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
            critical("Data format exception while trying to get the playlist status for a creature: {}", e.what());
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  e.what(),
                                  "Data format exception while loading the creature");
            return status;
        }
        catch (const creatures::InvalidArgumentException &e) {
            error("an empty creatureID was passed in while attempting to get the playlist status");
            status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                  e.what(),
                                  fmt::format("⚠️ A creature id must be supplied"));
            return status;
        }
        catch (...) {
            critical("unknown except"
                     "ion while trying to load a creature to get its playlist status");
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "Unknown exception while trying to load a creature to get its playlist status");
            return status;
        }

        // Hooray
        debug("the creature ID is valid! (it's {})", creature->name());


        try {
            auto runningList = runningPlaylists->get(creatureIdToString(*creatureId));

            debug("{} is currently playing playlist {}", creature->name(), playlistIdentifierToString(*runningList));
            response->set_playing(true);
            trace("set playing to true");

            // Avoid a SIGBUS by getting a local copy of this data
            playlistId.set__id(runningList->_id());
            response->mutable_playlistid()->CopyFrom(playlistId);
            status = grpc::Status(grpc::StatusCode::OK, "Currently playing {}",
                                  playlistIdentifierToString(playlistId));

            debug("successfully got a creature's status");
            return status;
        }
        catch (std::out_of_range &e) {
            debug("nothing currently running on creature {}", creature->name());
            response->set_playing(false);
            status = grpc::Status(grpc::StatusCode::OK, "Nothing currently playing");
            return status;
        }
        catch (...) {
            critical(
                    "unknown exception while trying to look at the running playlists while getting a creature's status");
            status = grpc::Status(grpc::StatusCode::INTERNAL,
                                  "Unknown exception while trying to look at the running playlists while getting a creature's status");
            return status;
        }

    }

}