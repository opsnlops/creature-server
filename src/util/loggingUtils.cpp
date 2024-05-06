

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <utility>

#include "concurrentqueue.h"

#include "server/logging/CreatureLogSink.h"
#include "util/MessageQueue.h"

namespace creatures {
    extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
}

namespace creatures {

    std::shared_ptr<spdlog::logger> makeLogger(std::string name, spdlog::level::level_enum defaultLevel) {

        // Console logger
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

        // Queue logger
        auto queue_sink = std::make_shared<spdlog::sinks::CreatureLogSink<std::mutex>>(websocketOutgoingMessages);

        // Make the logger
        auto logger = std::make_shared<spdlog::logger>(std::move(name), spdlog::sinks_init_list{console_sink, queue_sink});
        logger->set_level(defaultLevel);

        return logger;
    }

}