

#include <chrono>
#include <ctime>
#include <thread>

#include <bsoncxx/oid.hpp>
#include <fmt/format.h>
#include <google/protobuf/timestamp.pb.h>
#include <grpcpp/grpcpp.h>

#include "server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

#include "client/server-client.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using grpc::ClientWriter;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::CreatureId;
using server::Frame;
using server::FrameResponse;
using server::ListCreaturesResponse;
using server::CreatureFilter;

using spdlog::trace;
using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;



int main(int argc, char** argv) {

    auto console = spdlog::stdout_color_mt("console");
    spdlog::set_level(spdlog::level::trace);

    CreatureServerClient client(
            grpc::CreateChannel("10.3.2.11:6666", grpc::InsecureChannelCredentials()));

    info("connecting to server...");

    client.StreamLogs(LogLevel::debug);

    return 0;
}