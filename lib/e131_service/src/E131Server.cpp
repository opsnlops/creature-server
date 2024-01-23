

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

extern "C" {
    #include <e131.h>
}


#include "E131Server.h"

namespace creatures::e131 {

    void E131Server::init() {

        // Get our logger going
        logger = spdlog::stdout_color_mt("E131Server");
        logger->set_level(spdlog::level::trace);
        logger->info("E131Server Initialized");

    }


} // creatures::e131