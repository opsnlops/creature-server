
#include <string>

#include "MessageTypes.h"

namespace creatures::ws {

    std::string toString(MessageType type) {
        switch (type) {

            case MessageType::Database: return "database";
            case MessageType::LogMessage: return "log";
            case MessageType::ServerCounters: return "server-counters";
            case MessageType::Notice: return "notice";

            default: return "unknown";
        }
    }

} // creatures :: ws