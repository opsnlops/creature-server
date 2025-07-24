
#include <string>

#include "MessageTypes.h"

namespace creatures::ws {

std::string toString(MessageType type) {
    switch (type) {

    case MessageType::Database:
        return "database";
    case MessageType::LogMessage:
        return "log";
    case MessageType::ServerCounters:
        return "server-counters";
    case MessageType::Notice:
        return "notice";
    case MessageType::StreamFrame:
        return "stream-frame";
    case MessageType::VirtualStatusLights:
        return "status-lights";
    case MessageType::UpsertCreature:
        return "upsert-creature";
    case MessageType::BoardSensorReport:
        return "board-sensor-report";
    case MessageType::MotorSensorReport:
        return "motor-sensor-report";
    case MessageType::CacheInvalidation:
        return "cache-invalidation";
    case MessageType::PlaylistStatus:
        return "playlist-status";

    default:
        return "unknown";
    }
}

} // namespace creatures::ws