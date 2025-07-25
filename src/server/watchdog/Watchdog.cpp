

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include "server/database.h"
#include "util/StoppableThread.h"
#include "util/threadName.h"

#include "Watchdog.h"

namespace creatures {

using creatures::Database;

Watchdog::Watchdog(const std::shared_ptr<Database> &db) : db(db) {
    logger = spdlog::stdout_color_mt("watchdog");
    logger->set_level(spdlog::level::debug);

    logger->info("Watchdog created");
}

void Watchdog::start() {
    logger->info("starting the watchdog thread");
    StoppableThread::start();
}

void Watchdog::run() {
    setThreadName("watchdog::run");

    logger->info("watchdog thread running");

    while (!stop_requested.load()) {

        logger->trace("starting watchdog loop");

        // Check the database
        try {
            db->performHealthCheck();
        } catch (std::exception &e) {
            logger->error("Database healthcheck failed: {}", e.what());
        }

        // Sleep for a bit
        std::this_thread::sleep_for(std::chrono::seconds(WATCHDOG_SLEEP_SECONDS));
    }
}

} // namespace creatures
