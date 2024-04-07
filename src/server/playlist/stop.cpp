
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
    extern std::shared_ptr<ObjectCache<universe_t, PlaylistIdentifier>> runningPlaylists;

    /**
     * Stop any playlists running on a creature
     *
     * @param context ServerContext from gPRC
     * @param stopRequest the universe to stop the playlist on
     * @param response the response
     * @return a status for the end user
     */
    grpc::Status CreatureServerImpl::StopPlaylist(ServerContext *context,
                                                  const PlaylistStopRequest *stopRequest,
                                                  PlaylistResponse *response) {

        info("Stopping all playlists on universe {} due to a gRPC request", stopRequest->universe());

        grpc::Status status;

        // Remove the creature's key from the cache
        runningPlaylists->remove((universe_t)stopRequest->universe());
        trace("removed from map");


        std::string okayMessage = fmt::format("🎵 Stopped playlists on universe {}", stopRequest->universe());

        info(okayMessage);
        response->set_message(okayMessage);
        response->set_success(true);
        metrics->incrementPlaylistsStopped();

        status = grpc::Status(grpc::StatusCode::OK, okayMessage);
        return status;
    }

}