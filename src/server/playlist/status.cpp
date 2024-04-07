
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
    extern std::shared_ptr<ObjectCache<universe_t , PlaylistIdentifier>> runningPlaylists;


    Status CreatureServerImpl::GetPlaylistStatus(grpc::ServerContext *context,
                                                 const PlaylistRequest *playlistRequest,
                                                 PlaylistStatus *response) {

        debug("incoming playlist status request for universe {}", playlistRequest->universe());
        metrics->incrementPlaylistStatusRequests();

        grpc::Status status;

        // Used as a buffer
        PlaylistIdentifier playlistId = PlaylistIdentifier();


        try {
            auto runningList = runningPlaylists->get((universe_t)playlistRequest->universe());

            debug("Universe {} is currently playing playlist {}",
                  playlistRequest->universe(),
                  playlistIdentifierToString(*runningList));
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
            debug("nothing currently running on universe {}", playlistRequest->universe());
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