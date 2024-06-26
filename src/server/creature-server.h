
#pragma once


#include "server/database.h"
#include "server/logging/CreatureLogSink.h"

//using google::protobuf::Empty;

//using grpc::Server;
//using grpc::ServerBuilder;
//using grpc::ServerContext;
//using grpc::ServerReader;
//using grpc::ServerWriter;
//using grpc::Status;

//using server::CreatureServer;
//
//#include "server/namespace-stuffs.h"
//
//using moodycamel::ConcurrentQueue;
//
//namespace creatures {
//
//    class CreatureServerImpl : public CreatureServer::Service {
//    public:
//        explicit CreatureServerImpl(ConcurrentQueue<LogItem> &_queue) : log_queue(_queue) {};
//
//        Status SearchCreatures(ServerContext *context, const CreatureName *request, server::Creature *reply) override;
//        Status CreateCreature(ServerContext *context, const server::Creature *creature, DatabaseInfo *reply) override;
//        Status GetCreature(ServerContext *context, const CreatureId *id, server::Creature *reply) override;
//        Status ListCreatures(ServerContext *context, const CreatureFilter *filter, ListCreaturesResponse *response) override;
//        Status GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
//                               GetAllCreaturesResponse *response) override;
//        Status UpdateCreature(ServerContext *context, const server::Creature *creature, DatabaseInfo *reply) override;
//        //Status GetServerStatus(ServerContext* context, const Empty* request, ServerStatus* response) override;
//
//        /**
//         * Stream logs from the server to the client
//         */
//        Status StreamLogs(ServerContext* context, const LogFilter* request, ServerWriter<LogItem>* writer) override;
//
//        Status StreamFrames(ServerContext* context, ServerReader<StreamFrameData>* reader, StreamFrameDataResponse* response) override;
//
//       Status gRPCCreateAnimation(ServerContext *context, const Animation *animation, DatabaseInfo *reply);
//
//        //Status UpdateAnimation(ServerContext *context, const Animation *animation, DatabaseInfo *reply) override;
//
//        Status PlayAnimation(ServerContext *context, const PlayAnimationRequest *request, PlayAnimationResponse *response) override;
//
//        Status PlaySound(ServerContext *context, const PlaySoundRequest *request, PlaySoundResponse *response) override;
//
//        /*
//         * Playlists
//         */
//        Status CreatePlaylist(ServerContext *context, const Playlist *playlist, DatabaseInfo *reply) override;
//        Status ListPlaylists(ServerContext *context, const PlaylistFilter *request, ListPlaylistsResponse *response) override;
//        Status GetPlaylist(ServerContext *context, const PlaylistIdentifier *id, Playlist *playlist) override;
//        Status UpdatePlaylist(ServerContext *context, const Playlist *playlist, DatabaseInfo *reply) override;
//        Status StartPlaylist(ServerContext *context, const PlaylistRequest *request, PlaylistResponse *response) override;
//        Status StopPlaylist(ServerContext *context, const PlaylistStopRequest *stopRequest, PlaylistResponse *response) override;
//        Status GetPlaylistStatus(ServerContext *context, const PlaylistRequest *playlistRequest, PlaylistStatus *response) override;
//
//
//    private:
//        ConcurrentQueue<LogItem>& log_queue;
//    };
//
//}


