
#include <atomic>
#include <csignal>
#include <locale>
#include <memory>
#include <string>
#include <thread>


// spdlog
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"

// SDL
#include <SDL2/SDL.h>

// E131Sever
#include <E131Server.h>


// Our stuff
#include "server/config.h"
#include "server/config/CommandLine.h"
#include "server/config/Configuration.h"
#include "server/creature-server.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/logging/concurrentqueue.h"
#include "server/logging/creature_log_sink.h"
#include "server/metrics/counters.h"
#include "server/metrics/StatusLights.h"
#include "Version.h"
#include "util/cache.h"
#include "util/environment.h"
#include "util/threadName.h"
#include "watchdog/Watchdog.h"

#include "server/ws/App.h"

#include "server/namespace-stuffs.h"

using creatures::Database;
using creatures::EventLoop;
using creatures::MusicEvent;

using moodycamel::ConcurrentQueue;

namespace creatures {
    std::shared_ptr<Configuration> config{};
    std::shared_ptr<Database> db{};
    std::shared_ptr <creatures::e131::E131Server> e131Server;
    std::shared_ptr<EventLoop> eventLoop;

    /**
     * Only one playlist can be running on a universe at a time. This is because animation can
     * involve any creature in a universe, so it doesn't make sense to have more than one playing
     * at any one time.
     */
    std::shared_ptr<ObjectCache<universe_t, std::string>> runningPlaylists;


    std::shared_ptr<GPIO> gpioPins;
    std::shared_ptr<SystemCounters> metrics;
    std::shared_ptr<StatusLights> statusLights;
    const char* audioDevice;
    SDL_AudioSpec audioSpec;
    std::atomic<bool> serverShouldRun{true};
}


// Signal handler to stop the event loop
void signal_handler(int signal) {

    switch (signal) {
        case SIGINT:
            info("SIGINT! signalling that we should stop the server");
            creatures::serverShouldRun.store(false);
            break;
        case SIGTERM:
            info("SIGTERM! signalling that we should stop the server");
            creatures::serverShouldRun.store(false);
            break;
        default:
            info("signal {} received, ignoring", signal);
            break;
    }
}


int main(int argc, char **argv) {

    // Fire up the signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Set up our locale
    std::locale::global(std::locale("en_US.UTF-8"));

    // Get the version
    std::string version = fmt::format("{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR, CREATURE_SERVER_VERSION_PATCH);

    // Set up our thread name
    setThreadName(fmt::format("creature-server::main version {}", version));

    // Create our metric counters
    creatures::metrics = std::make_shared<creatures::SystemCounters>();

    // Console logger
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console_sink->set_level(spdlog::level::trace);

    // Queue logger
    //ConcurrentQueue<LogItem> log_queue;
    //auto queue_sink = std::make_shared<spdlog::sinks::CreatureLogSink<std::mutex>>(log_queue);
    //queue_sink->set_level(spdlog::level::trace);

    // There's no need to set a name, it's just noise
    spdlog::logger logger("", {console_sink /*, queue_sink*/});
    logger.set_level(spdlog::level::trace);

    // Take over the default logger with our new one
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(logger));


    // Parse out the command line options
    auto commandLine = std::make_unique<creatures::CommandLine>();
    creatures::config = commandLine->parseCommandLine(argc, argv);


    // Leave some version info to be found
    info("Creature Server version {}", version);
    debug("spdlog version {}.{}.{}", SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH);
    debug("fmt version {}", FMT_VERSION);
    debug("MongoDB C++ driver version {}", MONGOCXX_VERSION_STRING);
    debug("SDL version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    debug("Sound file location: {}", creatures::config->getSoundFileLocation());


    // Fire up the Mongo client
    std::string mongoURI = creatures::config->getMongoURI();
    debug("MongoDB URI: {}", mongoURI);

    mongocxx::instance instance{};
    mongocxx::uri uri(mongoURI);
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
    if(!MusicEvent::locateAudioDevice()) {
        error("unable to open audio device; halting");
        std::exit(EXIT_FAILURE);
    }

    // Fire up the GPIO
    debug("Bringing up the GPIO pins");
    creatures::gpioPins = std::make_shared<creatures::GPIO>();

    // Fire up the watchdog
    debug("Starting up the status lights");
    creatures::statusLights = std::make_shared<creatures::StatusLights>();
    creatures::statusLights->start();

    // Create the playlist cache
    creatures::runningPlaylists = std::make_shared<creatures::ObjectCache<universe_t, std::string>>();
    debug("Playlist cache made");

    // Start up the event loop
    creatures::eventLoop = std::make_shared<EventLoop>();
    creatures::eventLoop->start();

    // Seed the tick task
    auto tickEvent = std::make_shared<creatures::TickEvent>(TICK_TIME_FRAMES);
    creatures::eventLoop->scheduleEvent(tickEvent);

    // Signal that we're online
    creatures::gpioPins->serverOnline(true);

    // Bring the E131Server online
    creatures::e131Server = std::make_shared<creatures::e131::E131Server>();
    creatures::e131Server->init(creatures::config->getNetworkDevice(), version);
    creatures::e131Server->start();

    // TODO: Remove this, this is just for debugging. Universe 1000 is "production."
    //creatures::e131Server->createUniverse(1000);

    // Fire up the watchdog
    auto watchdog = std::make_shared<creatures::Watchdog>(creatures::db);
    watchdog->start();


    // Start the web server
    auto webServer = std::make_shared<creatures::ws::App>();
    webServer->start();


    // Wait for the signal handler to know when to stop
    while (creatures::serverShouldRun.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    /*
     * IT'S SHUT DOWN TIME! 🤪
     */

    info("starting shutdown process");

    // Tell the E131Server to stop
    creatures::e131Server->shutdown();

    // Tell the watchdog to stop
    watchdog->shutdown();

    // Halt the event loop
    creatures::eventLoop->shutdown();

    // Stop the websocket server
    webServer->shutdown();

    creatures::gpioPins->serverOnline(false);
    creatures::statusLights->shutdown();

    // Clean up SDL
    info("shutting down SDL");
    SDL_Quit();
    debug("SDL shut down");

    // This most likely isn't needed, but given that there's so many threads
    // running, I figure it's a good thing to do.
    debug("waiting for a few seconds to let everyone clean up");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    std::cout << "Bye! 🖖🏻" << std::endl;
    std::exit(EXIT_SUCCESS);
}