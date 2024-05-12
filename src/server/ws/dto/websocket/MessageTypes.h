
#pragma once

#include <string>

namespace creatures::ws {

    enum class MessageType {
        Database,
        LogMessage,
        ServerCounters,
        Notice,
        StreamFrame
    };

   // Don't forget to update the toString function in MessageTypes.cpp
   std::string toString(MessageType type);

} // creatures :: ws