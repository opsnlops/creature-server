
#include <memory>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <uWebSockets/App.h>

#include "server/ws/routes/AllCreatures.h"
#include "server/ws/routes/CreatureById.h"
#include "server/ws/routes/HelloRoute.h"
#include "util/StoppableThread.h"
#include "util/threadName.h"


#include "WebSocketServer.h"


namespace creatures::ws {


    WebSocketServer::WebSocketServer(uint16_t serverPort) : serverPort(serverPort) {

        // Get our logger going
        logger = spdlog::stdout_color_mt("WebSocketServer");

        // Extend the default format by added the thread ID
        logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] [thread %t] %v");

        logger->set_level(spdlog::level::debug);

    }

    void WebSocketServer::start() {
        creatures::StoppableThread::start();
    }


    void WebSocketServer::run() {
        setThreadName("WebSocketServer::run");
        logger->info("Firing off WebSocket server thread!");

        // Create the app
        uWS::App app;

        // Add our routes
        addRoute<HelloRoute>(app, logger);

        // Creatures routes
        addRoute<AllCreatures>(app, logger);
        addRoute<CreatureById>(app, logger);

        // Start the server on a specific port, check if successful
        app.listen(serverPort, [this](auto *listenSocket) {

            if (listenSocket) {
                this->logger->info("Listening on port {}", serverPort);
            } else {
                this->logger->error("Failed to load certs or to bind to port");
            }

        });


        // Determine the number of threads to use
        unsigned int numberOfThreads = std::thread::hardware_concurrency();
        logger->info("Using {} threads", numberOfThreads);

        // Run the app in multiple threads
        std::vector<std::thread> threads(numberOfThreads - 1);  // Create threads for all but the main thread
        for (std::thread &t : threads) {
            t = std::thread([&app]() {
                app.run();  // Each thread runs the app
            });
        }

        app.run();  // Run also on the main thread

        // Join all threads to ensure clean exit
        for (std::thread &t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }

        logger->info("Server threads joined");
    }

}
