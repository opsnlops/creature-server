
#include <fmt/format.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <uWebSockets/App.h>


#include "model/Creature.h"
#include "server/database.h"
#include "util/StoppableThread.h"
#include "util/threadName.h"

#include "HttpResponse.h"
#include "WebSocketServer.h"


namespace creatures::ws {


    extern std::shared_ptr<Database> db;

    WebSocketServer::WebSocketServer(uint16_t serverPort) : serverPort(serverPort) {

        // Get our logger going
        logger = spdlog::stdout_color_mt("WebSocketServer");
        logger->set_level(spdlog::level::debug);

    }

    void WebSocketServer::start() {
        creatures::StoppableThread::start();
    }


    void WebSocketServer::run() {

        setThreadName("WebSocketServer::run");

        logger->info("firing off WebSocket server thread!");

        auto app = uWS::App();

        app.get("/hello", [this](auto *res, auto *req) {

             logger->debug("someone said hello!");
             WebSocketServer::sendResponse(res, {HttpStatus::OK, "Hellorld!"});

        });

        app.get("/creature/:id", [](auto *res, auto */*req*/) {

            auto creature1 = creatures::Creature{"1234", "Beaky", 2, 1, "This is a note!"};
            auto creature2 = creatures::Creature{"4567", "Mango 😍", 2, 5, "This is a note, too"};

            std::vector<creatures::Creature> creatures = {creature1, creature2};
            WebSocketServer::sendResponse(res, {HttpStatus::OK, creatures});

        });

        app.listen(serverPort, [this](auto *listen_socket) {
            if (listen_socket) {
                logger->info("Listening on port {}", serverPort);
            }
        });

        app.run();


        // Wait until we're asked to stop
        while (!stop_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }


        logger->info("goodbye from the WebSocket server!");
    }


    void WebSocketServer::sendResponse(uWS::HttpResponse<false> *res, const HttpResponse& response) {
        auto [code, message] = getHttpStatusMessage(response.getStatus());
        std::string status = std::to_string(code) + " " + message;
        res->writeStatus(status);
        res->writeHeader("Content-Type", "application/json; charset=utf-8");
        res->end(response.getBody());
    }

}
