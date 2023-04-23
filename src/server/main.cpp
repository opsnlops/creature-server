
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>
#include "messaging/server.grpc.pb.h"

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/common.h"


#include "server/creature-server.h"
#include "server/database.h"
#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"

#include "exception/exception.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::DatabaseInfo;
using server::LogItem;

using spdlog::info;
using spdlog::debug;
using spdlog::critical;
using spdlog::error;

using creatures::Database;

using moodycamel::ConcurrentQueue;


Database *db{};


void RunServer(uint16_t port, ConcurrentQueue<LogItem> &log_queue) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    creatures::CreatureServerImpl service(log_queue);

    ServerBuilder builder;
    // Listen on the given address without any authentication mechanism.
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    // Register "service" as the instance through which we'll communicate with
    // clients. In this case it corresponds to an *synchronous* service.
    builder.RegisterService(&service);
    // Finally assemble the server.
    std::unique_ptr<Server> server(builder.BuildAndStart());
    info("Server listening on {}", server_address);

    server->Wait();
    info("Bye!");
}

int main(int argc, char **argv) {

    // Configure logging

    // Console logger
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);

    // Queue logger
    ConcurrentQueue<LogItem> log_queue;
    auto queue_sink = std::make_shared<spdlog::sinks::CreatureLogSink<std::mutex>>(log_queue);
    queue_sink->set_level(spdlog::level::trace);

    // There's no need to set a name, it's just noise
    spdlog::logger logger("", {console_sink, queue_sink});
    logger.set_level(spdlog::level::trace);

    // Take over the default logger with our new one
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));


    debug("Hello from spdlog version {}.{}.{}", SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH);

    // Fire up the Mono client
    mongocxx::instance instance{};
    mongocxx::uri uri(DB_URI);
    mongocxx::pool mongo_pool(uri);

    // Start up the database
    db = new Database(mongo_pool);


    info("starting server on port {}", 6666);
    RunServer(6666, log_queue);
    return 0;
}