
#include "blockingconcurrentqueue.h"
#include "spdlog/spdlog.h"
#include <oatpp/core/macro/component.hpp>


#include "SensorReportHandler.h"

namespace creatures {
    extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
}


namespace creatures ::ws {

    void SensorReportHandler::processMessage(const oatpp::String &message) {

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

        appLogger->debug("processing an incoming SensorReport message");

        // There's not a lot to do for this one besides send it to all the currently connected clients
        websocketOutgoingMessages->enqueue(message);

    }
}