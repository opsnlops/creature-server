
#pragma once

#include <grpcpp/grpcpp.h>

#include "absl/strings/str_format.h"

#include "messaging/server.pb.h"
#include "messaging/server.grpc.pb.h"

#include "server/database.h"
#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"

using google::protobuf::Empty;

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerWriter;
using grpc::Status;

using server::Animation;
using server::AnimationFilter;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::CreatureId;
using server::CreatureFilter;
using server::DatabaseInfo;
using server::Frame;
using server::FrameResponse;
using server::GetAllCreaturesResponse;
using server::ListAnimationsResponse;
using server::ListCreaturesResponse;
using server::LogFilter;
using server::LogItem;
using server::PlayAnimationRequest;
using server::PlayAnimationResponse;
using server::PlaySoundRequest;
using server::PlaySoundResponse;
using server::ServerStatus;


using moodycamel::ConcurrentQueue;

namespace creatures {

    class CreatureServerImpl : public CreatureServer::Service {
    public:
        explicit CreatureServerImpl(ConcurrentQueue<LogItem> &_queue) : log_queue(_queue) {};

        Status SearchCreatures(ServerContext *context, const CreatureName *request, Creature *reply) override;
        Status CreateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) override;
        Status GetCreature(ServerContext *context, const CreatureId *id, Creature *reply) override;
        Status ListCreatures(ServerContext *context, const CreatureFilter *filter, ListCreaturesResponse *response) override;
        Status GetAllCreatures(ServerContext *context, const CreatureFilter *filter,
                               GetAllCreaturesResponse *response) override;
        Status UpdateCreature(ServerContext *context, const Creature *creature, DatabaseInfo *reply) override;
        //Status GetServerStatus(ServerContext* context, const Empty* request, ServerStatus* response) override;

        /**
         * Stream logs from the server to the client
         */
        Status StreamLogs(ServerContext* context, const LogFilter* request, ServerWriter<LogItem>* writer) override;

        Status StreamFrames(ServerContext* context, ServerReader<Frame>* reader, FrameResponse* response) override;

        Status CreateAnimation(ServerContext *context, const Animation *animation, DatabaseInfo *reply) override;

        Status UpdateAnimation(ServerContext *context, const Animation *animation, DatabaseInfo *reply) override;

        Status ListAnimations(ServerContext *context, const AnimationFilter *request, ListAnimationsResponse *response) override;

        Status GetAnimation(ServerContext *context, const AnimationId *id, Animation *animation) override;

        Status PlayAnimation(ServerContext *context, const PlayAnimationRequest *request, PlayAnimationResponse *response) override;

        Status PlaySound(ServerContext *context, const PlaySoundRequest *request, PlaySoundResponse *response) override;

    private:
        ConcurrentQueue<LogItem>& log_queue;
    };
    
}


