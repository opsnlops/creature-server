
#pragma once

#include "spdlog/spdlog.h"

#include "server/database.h"
#include "util/StoppableThread.h"
#include "util/threadName.h"


#define WATCHDOG_SLEEP_SECONDS 2

namespace creatures {

    /**
     * This class is a watchdog that monitors the health of the system. At the moment
     * its main use case is making sure that the database is alive and working.
     */
    class Watchdog : public StoppableThread {

    public:
        Watchdog(std::shared_ptr<creatures::Database> db);

        ~Watchdog() override {
            logger->info("Watchdog destroyed");
        }

        void start() override;

    protected:
        void run() override;

    private:

        // Our private logger
        std::shared_ptr<spdlog::logger> logger;

        std::shared_ptr<creatures::Database> db;

    };

} // creatures


