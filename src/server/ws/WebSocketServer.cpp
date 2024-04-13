
#include <uWebSockets/App.h>

#include "WebSocketServer.h"

void testWebSockets() {
    /* Overly simple hello world app */
    uWS::SSLApp({
                        .key_file_name = "misc/key.pem",
                        .cert_file_name = "misc/cert.pem",
                        .passphrase = "1234"
                }).get("/*", [](auto *res, auto */*req*/) {
        res->end("Hello world!");
    }).listen(3000, [](auto *listen_socket) {
        if (listen_socket) {
            std::cout << "Listening on port " << 3000 << std::endl;
        }
    }).run();
}