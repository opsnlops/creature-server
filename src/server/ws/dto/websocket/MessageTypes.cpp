
#include <string>

#include "MessageTypes.h"

namespace creatures::ws {

    std::string toString(MessageType type) {
        switch (type) {

            case MessageType::Database: return "database";
            case MessageType::LogMessage: return "log";
            case MessageType::ServerCounters: return "server-counters";
            case MessageType::Notice: return "notice";
            case MessageType::StreamFrame: return "stream-frame";
            case MessageType::VirtualStatusLights: return "status-lights";
            case MessageType::UpsertCreature: return "upsert-creature";
            case MessageType::CreatureSensorReport: return "sensor-report";

            default: return "unknown";
        }
    }

} // creatures :: ws