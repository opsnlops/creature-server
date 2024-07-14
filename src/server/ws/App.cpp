
#include <iostream>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <oatpp/network/Server.hpp>
//#include <oatpp-swagger/Controller.hpp>

#include "util/threadName.h"
#include "util/StoppableThread.h"


#include "App.h"
#include "AppComponent.h"
#include "SwaggerComponent.h"
#include "controller/AnimationController.h"
#include "controller/CreatureController.h"
#include "controller/DebugController.h"
#include "controller/MetricsController.h"
#include "controller/PlaylistController.h"
#include "controller/SoundController.h"
#include "controller/StaticController.h"
#include "controller/VoiceController.h"
#include "controller/WebSocketController.h"

#include "server/ws/websocket/ClientCafe.h"
#include "util/loggingUtils.h"

namespace creatures ::ws {


    App::App() {
        //internalLogger = spdlog::stdout_color_mt("web-server-system");
        //internalLogger->set_level(spdlog::level::debug);
        internalLogger = creatures::makeLogger("web-server-system", spdlog::level::debug);
        internalLogger->info("web-server created");
    }

    void App::start() {

        internalLogger->info("starting the web server");
        oatpp::base::Environment::init();



        internalLogger->debug("firing up the web-server's worker thread");
        creatures::StoppableThread::start();
    }

    void App::run() {

        setThreadName("web-server::run");

        AppComponent components; // Create scope Environment components

        /* Get router component */
        OATPP_COMPONENT(std::shared_ptr<oatpp::web::server::HttpRouter>, router);

        // Register our logger in the environment
        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

//        oatpp::web::server::api::Endpoints docEndpoints;
//
//        docEndpoints.append(router->addController(AnimationController::createShared())->getEndpoints());
//        docEndpoints.append(router->addController(CreatureController::createShared())->getEndpoints());
//        docEndpoints.append(router->addController(MetricsController::createShared())->getEndpoints());
//        docEndpoints.append(router->addController(SoundController::createShared())->getEndpoints());
//        docEndpoints.append(router->addController(WebSocketController::createShared())->getEndpoints());
//        router->addController(oatpp::swagger::Controller::createShared(docEndpoints));

        router->addController(AnimationController::createShared());
        router->addController(CreatureController::createShared());
        router->addController(DebugController::createShared());
        router->addController(MetricsController::createShared());
        router->addController(PlaylistController::createShared());
        router->addController(SoundController::createShared());
        router->addController(StaticController::createShared());
        router->addController(VoiceController::createShared());
        router->addController(WebSocketController::createShared());

        /* Get connection handler component */
        OATPP_COMPONENT(std::shared_ptr<oatpp::network::ConnectionHandler>, connectionHandler, "rest");

        /* Get connection provider component */
        OATPP_COMPONENT(std::shared_ptr<oatpp::network::ServerConnectionProvider>, connectionProvider);

        // Run the ping loop
        std::thread pingThread([]{
            OATPP_COMPONENT(std::shared_ptr<ClientCafe>, cafe);
            cafe->runPingLoop(std::chrono::seconds(30));
        });

        // Run the message processing loop
        std::thread queueWorkerThread([]{
            OATPP_COMPONENT(std::shared_ptr<ClientCafe>, cafe);
            cafe->runMessageLoop();
        });

        /* create server */
        oatpp::network::Server server(connectionProvider,
                                      connectionHandler);

        appLogger->info("Running on port {}", connectionProvider->getProperty("port").toString()->c_str());
        server.run();

        /* stop db connection pool */
        //OATPP_COMPONENT(std::shared_ptr<oatpp::provider::Provider<oatpp::sqlite::Connection>>, dbConnectionProvider);
        //dbConnectionProvider->stop();

    }

}