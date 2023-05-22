
#include <atomic>
#include <csignal>
#include <locale>
#include <memory>
#include <mutex>
#include <string>

// gRPC
#include "absl/strings/str_format.h"
#include <grpcpp/grpcpp.h>
#include "messaging/server.grpc.pb.h"

// spdlog
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// SDL
#include <SDL2/SDL.h>

// Our stuff
#include "server/config.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/dmx/dmx.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"
#include "util/cache.h"


using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using server::CreatureServer;
using server::Creature;
using server::CreatureName;
using server::DatabaseInfo;
using server::LogItem;

using spdlog::trace;
using spdlog::debug;
using spdlog::info;
using spdlog::warn;
using spdlog::error;
using spdlog::critical;

using creatures::Database;
using creatures::EventLoop;
using creatures::MusicEvent;

using moodycamel::ConcurrentQueue;

namespace creatures {
    std::atomic<bool> eventLoopRunning{true};
    std::shared_ptr<Database> db{};
    std::unique_ptr<Server> grpcServer;
    std::shared_ptr<EventLoop> eventLoop;
    std::shared_ptr<ObjectCache<std::string, DMX>> dmxCache;
    SDL_AudioDeviceID audioDevice;
    SDL_AudioSpec audioSpec;
}


// Signal handler to stop the event loop
void signal_handler(int signal) {
    if (signal == SIGINT) {
        info("stopping the event loop");
        creatures::eventLoopRunning = false;

        // Clean up SDL
        info("shutting down SDL");
        SDL_Quit();

        // If the server is running, stop it
        if(creatures::grpcServer) {
            info("stopping the gRPC service");
            creatures::grpcServer->Shutdown(std::chrono::system_clock::now());
            creatures::grpcServer.reset();
        }
    }
}


void RunServer(uint16_t port, ConcurrentQueue<LogItem> &log_queue) {
    std::string server_address = absl::StrFormat("0.0.0.0:%d", port);
    creatures::CreatureServerImpl service(log_queue);

    ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // 🚜 Build and start!
    creatures::grpcServer = builder.BuildAndStart();
    info("Server listening on {}", server_address);

    creatures::grpcServer->Wait();
    info("Bye!");
}


int main([[maybe_unused]] int argc, [[maybe_unused]] char **argv) {

    // Fire up the signal handlers
    std::signal(SIGINT, signal_handler);

    // Set up our locale
    std::locale::global(std::locale("en_US.UTF-8"));

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

    // Leave some version info to be found
    debug("spdlog version {}.{}.{}", SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH);
    debug("fmt version {}", FMT_VERSION);
    debug("MongoDB C++ driver version {}", MONGOCXX_VERSION_STRING);
    debug("MongoDB URI {}", DB_URI);
    debug("gRPC version {}.{}.{}", GRPC_CPP_VERSION_MAJOR, GRPC_CPP_VERSION_MINOR, GRPC_CPP_VERSION_PATCH);
    debug("Protobuf version {}", GOOGLE_PROTOBUF_VERSION);
    debug("SDL version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    debug("Sound file location: {}", MusicEvent::getSoundFileLocation());

    // Fire up the Mono client
    mongocxx::instance instance{};
    mongocxx::uri uri(DB_URI);
    mongocxx::pool mongo_pool(uri);

    // Start up the database
    creatures::db = std::make_shared<Database>(mongo_pool);
    debug("Mongo pool up and running");

    // Fire up SDL
    if(!MusicEvent::initSDL()) {
        error("Unable to start up SDL");
    }
    debug("SDL started");
    MusicEvent::listAudioDevices();
    if(!MusicEvent::openAudioDevice()) {
        error("unable to open audio device; halting");
        return 0;
    }

    // Create the DMX cache
    creatures::dmxCache = std::make_shared<creatures::ObjectCache<std::string, creatures::DMX>>();
    debug("DMX cache made");

    // Start up the event loop
    creatures::eventLoop = std::make_unique<EventLoop>();
    creatures::eventLoop->run();

    // Seed the tick task
    auto tickEvent = std::make_shared<creatures::TickEvent>(TICK_TIME_FRAMES);
    creatures::eventLoop->scheduleEvent(tickEvent);



    info("starting server on port {}", 6666);
    RunServer(6666, log_queue);

    return 0;
}