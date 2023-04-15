

#include "spdlog/spdlog.h"

#include "exception/exception.h"
#include "server/creature-server.h"


using spdlog::info;
using spdlog::debug;
using spdlog::error;
using spdlog::critical;
using spdlog::trace;

/**
 * Streams our logs to a client for viewing
 *
 * This is mostly used to allow the Creature Console to show the current state of the
 * server within the application.
 *
 */
Status creatures::CreatureServerImpl::StreamLogs(ServerContext* context,
                                                 const LogFilter* request,
                                                 ServerWriter<LogItem>* writer) {
    info("request to stream logs received");

    LogItem logItem;

    int requestedLevel = request->level();

    while (!context->IsCancelled()) {
        if (log_queue.try_dequeue(logItem)) {

            // If this message is equal to, or higher than the log level the requested,
            // send it down the wire to it.
            if(logItem.level() >= requestedLevel)
                writer->Write(logItem);

        } else {

            // Sleep for a bit before checking again
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    info("done streaming logs");
    return Status::OK;

}