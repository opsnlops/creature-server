
#pragma once

#include "spdlog/spdlog.h"

#include <uWebSockets/App.h>

#include "server/namespace-stuffs.h"

#include "util/StoppableThread.h"

#include "HttpResponse.h"

namespace creatures :: ws {

    class WebSocketServer : public StoppableThread {

    public:
        WebSocketServer(uint16_t serverPort);

        ~WebSocketServer() = default;

        void start() override;


        /**
         * Send a response to the client
         *
         * @param res The response object
         * @param response The response to send
         */
        static void sendResponse(uWS::HttpResponse<false> *res, const HttpResponse& response);

    protected:
        void run() override;


    private:
        uint16_t serverPort;
        std::shared_ptr<spdlog::logger> logger;
    };
}