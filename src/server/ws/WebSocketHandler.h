
#pragma once

#include "server/ws/WebSocketServer.h"

#include "routes/BaseRoute.h"


namespace creatures ::ws {

    struct PerSocketData {
        /* Fill with user data */
        int something;
    };

    class WebSocketHandler : public BaseRoute {


    public:
        explicit WebSocketHandler(std::shared_ptr<spdlog::logger> logger) : BaseRoute(logger) {}

        void registerRoute(uWS::App &app) override {

            logger->debug("Registering the WebSocket handler on /ws/v1");

            app.ws<PerSocketData>("/ws/v1", {
                    /* Settings */
                    .compression = uWS::CompressOptions(uWS::DEDICATED_COMPRESSOR_4KB | uWS::DEDICATED_DECOMPRESSOR),
                    .maxPayloadLength = 100 * 1024 * 1024,
                    .idleTimeout = 16,
                    .maxBackpressure = 100 * 1024 * 1024,
                    .closeOnBackpressureLimit = false,
                    .resetIdleTimeoutOnSend = false,
                    .sendPingsAutomatically = true,
                    /* Handlers */
                    .upgrade = [](auto *res, auto *req, auto *context) {

                        /* You may read from req only here, and COPY whatever you need into your PerSocketData.
                         * PerSocketData is valid from .open to .close event, accessed with ws->getUserData().
                         * HttpRequest (req) is ONLY valid in this very callback, so any data you will need later
                         * has to be COPIED into PerSocketData here. */

                        /* Immediately upgrading without doing anything "async" before, is simple */
                        res->template upgrade<PerSocketData>({
                                                                     /* We initialize PerSocketData struct here */
                                                                     .something = 13
                                                             }, req->getHeader("sec-websocket-key"),
                                                             req->getHeader("sec-websocket-protocol"),
                                                             req->getHeader("sec-websocket-extensions"),
                                                             context);

                        /* If you don't want to upgrade you can instead respond with custom HTTP here,
                         * such as res->writeStatus(...)->writeHeader(...)->end(...); or similar.*/

                        /* Performing async upgrade, such as checking with a database is a little more complex;
                         * see UpgradeAsync example instead. */
                    },
                    .open = [](auto */*ws*/) {
                        /* Open event here, you may access ws->getUserData() which points to a PerSocketData struct */

                    },
                    .message = [this](auto *ws, std::string_view message, uWS::OpCode opCode) {
                        /* This is the opposite of what you probably want; compress if message is LARGER than 16 kb
                         * the reason we do the opposite here; compress if SMALLER than 16 kb is to allow for
                         * benchmarking of large message sending without compression */
                        ws->send(message, opCode, message.length() < 16 * 1024);
                        logger->info("got a message: {}", message);
                    },
                    .dropped = [](auto */*ws*/, std::string_view /*message*/, uWS::OpCode /*opCode*/) {
                        /* A message was dropped due to set maxBackpressure and closeOnBackpressureLimit limit */
                    },
                    .drain = [](auto */*ws*/) {
                        /* Check ws->getBufferedAmount() here */
                    },
                    .ping = [this](auto */*ws*/, std::string_view) {
                        /* Not implemented yet */
                        logger->debug("ping");
                    },
                    .pong = [this](auto */*ws*/, std::string_view) {
                        /* Not implemented yet */
                        logger->debug("pong");
                    },
                    .close = [](auto */*ws*/, int /*code*/, std::string_view /*message*/) {
                        /* You may access ws->getUserData() here */
                    }
            });
        }

    };


}