
#include "spdlog/spdlog.h"

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>


#include <mongocxx/client.hpp>
#include <mongocxx/exception/bulk_write_exception.hpp>


#include "server/animation/animation.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "exception/exception.h"


using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

namespace creatures {

    extern std::shared_ptr<Database> db;
    extern std::shared_ptr<EventLoop> eventLoop;

    grpc::Status CreatureServerImpl::PlayAnimation(grpc::ServerContext *context, const server::PlayAnimationRequest *request,
                                             server::PlayAnimationResponse *response) {

        info("Playing an animation from a gRPC request");
        return grpc::Status::OK;
    }

}