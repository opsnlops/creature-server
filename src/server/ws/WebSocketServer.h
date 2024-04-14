
#pragma once

#include <vector>

#include "server/ws/routes/BaseRoute.h"
#include "spdlog/spdlog.h"
#include <uWebSockets/App.h>

#include "util/StoppableThread.h"

#include "HttpResponse.h"

namespace creatures ::ws {

    class WebSocketServer : public StoppableThread {

    public:

        WebSocketServer(uint16_t serverPort);

        ~WebSocketServer() = default;

        void start() override;


        template<typename RouteType, typename... Args>
        requires RouteConcept<RouteType>
        void addRoute(uWS::App &app, Args &&... args) {
            auto route = std::make_unique<RouteType>(std::forward<Args>(args)...);
            route->registerRoute(app);
            routes.push_back(std::move(route));
        }


        /**
         * Send a response to the client
         *
         * @param res The response object
         * @param response The response to send
         */
        static void sendResponse(uWS::HttpResponse<false> *res, const HttpResponse &response) {
            auto [code, message] = getHttpStatusMessage(response.getStatus());
            std::string status = std::to_string(code) + " " + message;
            res->writeStatus(status);
            res->writeHeader("Content-Type", "application/json; charset=utf-8");
            res->end(response.getBody());

        }

    protected:
        void run() override;


    private:

        uint16_t serverPort;
        std::shared_ptr<spdlog::logger> logger;

        // Maintain a list of routes
        std::vector<std::unique_ptr<BaseRoute>> routes;
    };
}