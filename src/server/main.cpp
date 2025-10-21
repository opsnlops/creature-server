//
// main.cpp
//

#include <atomic>
#include <csignal>
#include <locale>
#include <memory>
#include <string>
#include <thread>

// spdlog
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/spdlog.h"

// SDL
#include <SDL2/SDL.h>

// E131Sever
#include <E131Server.h>

// MoodyCamel
#include "blockingconcurrentqueue.h"

// uvgrtp
#include "uvgrtp/version.hh"

// Opus
#include <opus/opus.h>

// Our stuff
#include "Version.h"
#include "model/PlaylistStatus.h"
#include "server/animation/SessionManager.h"
#include "server/config.h"
#include "server/config/CommandLine.h"
#include "server/config/Configuration.h"
#include "server/database.h"
#include "server/eventloop/eventloop.h"
#include "server/eventloop/events/types.h"
#include "server/gpio/gpio.h"
#include "server/metrics/StatusLights.h"
#include "server/metrics/counters.h"
#include "server/rtp/AudioStreamBuffer.h"
#include "server/rtp/MultiOpusRtpServer.h"
#include "server/sensors/SensorDataCache.h"
#include "util/AudioCache.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"
#include "util/loggingUtils.h"
#include "util/threadName.h"
#include "util/websocketUtils.h"
#include "watchdog/Watchdog.h"

#include "server/ws/App.h"

using creatures::Database;
using creatures::EventLoop;
using creatures::MusicEvent;

namespace creatures {
std::shared_ptr<Configuration> config{};
std::shared_ptr<Database> db{};
std::shared_ptr<e131::E131Server> e131Server;
std::shared_ptr<EventLoop> eventLoop;

/**
 * Only one playlist can be running on a universe at a time. This is because animation can
 * involve any creature in a universe, so it doesn't make sense to have more than one playing
 * at any one time.
 */
std::shared_ptr<ObjectCache<universe_t, PlaylistStatus>> runningPlaylists;

/**
 * Maintain a cache of the creatures. While going to the DB is very quick, there's some operations
 * that require looking them up over and over again, like streaming frames. Rather than going back
 * to the database each time, let's keep a cache of them on hand.
 */
std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;

/**
 * Maps creature IDs to their currently assigned universe. This is runtime-only state that tracks
 * which universe a creature is operating on when its controller registers. This is NOT persisted
 * to the database - the creature config file on the controller is the source of truth.
 */
std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

std::shared_ptr<GPIO> gpioPins;
std::shared_ptr<SystemCounters> metrics;
std::shared_ptr<StatusLights> statusLights;
const char *audioDevice;
SDL_AudioSpec localAudioDeviceAudioSpec;
std::atomic serverShouldRun{true};

// MoodyCamel queue for outgoing websocket messages
std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;

// Observability manager for tracing and metrics
std::shared_ptr<ObservabilityManager> observability;

// RTP server for handling real-time protocol streaming
std::shared_ptr<rtp::MultiOpusRtpServer> rtpServer;

// Audio cache for pre-encoded Opus files
std::shared_ptr<util::AudioCache> audioCache;

// Sensor data cache for storing current sensor readings from creatures
std::shared_ptr<SensorDataCache> sensorDataCache;

// Session manager for tracking active playback and handling interrupts
std::shared_ptr<class SessionManager> sessionManager;
} // namespace creatures

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

int main(const int argc, char **argv) {

    // Fire up the signal handlers
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Set up our locale
    std::locale::global(std::locale("en_US.UTF-8"));

    // Get the version
    std::string version = fmt::format("{}.{}.{}", CREATURE_SERVER_VERSION_MAJOR, CREATURE_SERVER_VERSION_MINOR,
                                      CREATURE_SERVER_VERSION_PATCH);

    // Set up our thread name
    setThreadName(fmt::format("creature-server::main version {}", version));

    // Create our metric counters
    creatures::metrics = std::make_shared<creatures::SystemCounters>();

    // Bring up the websocket outgoing queue
    creatures::websocketOutgoingMessages = std::make_shared<moodycamel::BlockingConcurrentQueue<std::string>>();

    // Make a logger that goes to the console and websocket clients
    const auto logger = creatures::makeLogger("main", spdlog::level::debug);

    // Take over the default logger with our new one
    spdlog::set_default_logger(logger);

    // Parse out the command line options
    const auto commandLine = std::make_unique<creatures::CommandLine>();
    creatures::config = commandLine->parseCommandLine(argc, argv);

    // Leave some version info to be found
    info("Creature Server version {}", version);
    debug("spdlog version {}.{}.{}", SPDLOG_VER_MAJOR, SPDLOG_VER_MINOR, SPDLOG_VER_PATCH);
    debug("fmt version {}", FMT_VERSION);
    debug("SDL version {}.{}.{}", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    debug("Sound file location: {}", creatures::config->getSoundFileLocation());
    debug("uvgrtp version {}", uvgrtp::get_version());
    debug("opus version {}", opus_get_version_string());

    // Create the observability manager
    creatures::observability = std::make_shared<creatures::ObservabilityManager>();
    creatures::observability->initialize(
        "creature-server",                       // service name
        version,                                 // service version
        creatures::config->getHoneycombApiKey(), // Honeycomb API key (empty = use local collector)
        "creature-server"                        // Honeycomb dataset name
    );
    debug("Observability manager initialized");

    // Fire up the Mongo client
    std::string mongoURI = creatures::config->getMongoURI();
    debug("MongoDB URI: {}", mongoURI);

    // Start up the database
    mongocxx::instance instance{}; // Make sure the client is ready to go
    creatures::db = std::make_shared<Database>(mongoURI);
    debug("MongoDB connection created");

    // Fire up SDL
    if (!MusicEvent::initSDL()) {
        error("Unable to start up SDL");
    }
    debug("SDL started");
    MusicEvent::listAudioDevices();
    if (!MusicEvent::locateAudioDevice()) {
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
    creatures::runningPlaylists = std::make_shared<creatures::ObjectCache<universe_t, creatures::PlaylistStatus>>();
    debug("Playlist cache made");

    // Create the Creature cache
    creatures::creatureCache = std::make_shared<creatures::ObjectCache<creatureId_t, creatures::Creature>>();
    debug("Created the creature cache");

    // Create the creature-to-universe mapping cache
    creatures::creatureUniverseMap = std::make_shared<creatures::ObjectCache<creatureId_t, universe_t>>();
    debug("Created the creature-to-universe mapping cache");

    // Create the sensor data cache
    creatures::sensorDataCache = std::make_shared<creatures::SensorDataCache>();
    debug("Created the sensor data cache");

    // Create the session manager for interrupt handling
    creatures::sessionManager = std::make_shared<creatures::SessionManager>();
    debug("Created the session manager");

    // Start up the event loop
    creatures::eventLoop = std::make_shared<EventLoop>();
    creatures::eventLoop->start();

    // Seed the tick task
    const auto tickEvent = std::make_shared<creatures::TickEvent>(TICK_TIME_FRAMES);
    creatures::eventLoop->scheduleEvent(tickEvent);

    // Signal that we're online
    creatures::gpioPins->serverOnline(true);

    // Start the RtpServer
    if (creatures::config->getAudioMode() == creatures::Configuration::AudioMode::RTP) {
        info("RTP audio mode enabled, starting RTP server");
        creatures::rtpServer = std::make_shared<creatures::rtp::MultiOpusRtpServer>();
    }

    // Initialize audio cache for faster Opus encoding
    try {
        creatures::audioCache =
            std::make_shared<creatures::util::AudioCache>(creatures::config->getSoundFileLocation());
        creatures::rtp::AudioStreamBuffer::setAudioCacheInstance(creatures::audioCache);
        info("Audio cache initialized for faster Opus encoding");

        auto stats = creatures::audioCache->getStats();
        debug("Audio cache stats: {} cached files, {} hits, {} misses", stats.totalCachedFiles, stats.cacheHits,
              stats.cacheMisses);
    } catch (const std::exception &e) {
        error("Failed to initialize audio cache: {}", e.what());
        warn("Audio will be encoded without caching (slower performance)");
        creatures::audioCache = nullptr;
        creatures::rtp::AudioStreamBuffer::setAudioCacheInstance(nullptr);
    }

    // Bring the E131Server online
    creatures::e131Server = std::make_shared<creatures::e131::E131Server>();
    creatures::e131Server->init(creatures::config->getNetworkDevice(), version);
    creatures::e131Server->start();

    // TODO: Remove this, this is just for debugging. Universe 1000 is "production."
    // creatures::e131Server->createUniverse(1000);

    // Fire up the watchdog
    auto watchdog = std::make_shared<creatures::Watchdog>(creatures::db);
    watchdog->start();

    // Start the web server
    auto webServer = std::make_shared<creatures::ws::App>();
    webServer->start();

    // Seed the metric send task
    const auto metricSendEvent = std::make_shared<creatures::CounterSendEvent>(SEND_COUNTERS_FRAMES);
    creatures::eventLoop->scheduleEvent(metricSendEvent);

    // Wait for the signal handler to know when to stop
    while (creatures::serverShouldRun.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }

    /*
     * IT'S SHUT DOWN TIME! 🤪
     */

    info("starting shutdown process");

    // Inform all currently connected clients we're stopping and then wait a second
    // to make sure that it goes out!
    creatures::broadcastNoticeToAllClients("Server is shutting down");
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // Tell the E131Server to stop
    creatures::e131Server->shutdown();

    // Tell the watchdog to stop
    watchdog->shutdown();

    // Stop the websocket server FIRST (before event loop)
    // This prevents web server threads from trying to use the event loop after it's destroyed
    webServer->shutdown();

    // Halt the event loop
    creatures::eventLoop->shutdown();

    // Cleanup the RTP server
    creatures::rtpServer.reset(); // implicit cleanup

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