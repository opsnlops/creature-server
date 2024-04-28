
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
    extern std::shared_ptr<ObjectCache<universe_t, std::string>> runningPlaylists;

//    grpc::Status CreatureServerImpl::StartPlaylist(ServerContext *context,
//                                                   const PlaylistRequest *request,
//                                                   PlaylistResponse *response) {
//
//        info("Starting a playlist from a gRPC request");
//
//        grpc::Status status;
//
//        auto playlistId = request->playlistid();
//        auto universe = request->universe();
//
//        // Load the playlist
//        auto playlist = std::make_shared<Playlist>();
//        try {
//            db->getPlaylist(&playlistId, playlist.get());
//            debug("loaded playlist {}", playlist->name());
//        }
//        catch (const NotFoundException &e) {
//            info("playlist not found");
//            status = grpc::Status(grpc::StatusCode::NOT_FOUND,
//                                  e.what(),
//                                  fmt::format("🚫 Playlist id not found"));
//            return status;
//        }
//        catch (const DataFormatException &e) {
//            critical("Data format exception while loading a playlist on animation playback: {}", e.what());
//            status = grpc::Status(grpc::StatusCode::INTERNAL,
//                                  e.what(),
//                                  "Data format exception while loading a playlist on animation playback");
//            return status;
//        }
//
//
//
//        // Set the playlist in the cache
//        runningPlaylists->put(universe, playlistId);
//
//        auto playEvent = std::make_shared<PlaylistEvent>(eventLoop->getNextFrameNumber(), universe);
//        eventLoop->scheduleEvent(playEvent);
//
//        std::string okayMessage = fmt::format("🎵 Started playing playlist {} on universe {}", playlist->name(), universe);
//
//        info(okayMessage);
//        response->set_message(okayMessage);
//        response->set_success(true);
//        metrics->incrementPlaylistsStarted();
//
//        status = grpc::Status(grpc::StatusCode::OK,
//                              okayMessage);
//        return status;
//    }

}