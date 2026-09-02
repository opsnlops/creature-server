
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "IMessageHandler.h"

namespace creatures ::ws {

struct WebSocketMessageMetadata {
    std::string transportFramework;
    uint64_t connectionId{0};
    uint64_t sequence{0};
    std::string triggerTraceId;
    std::string triggerSpanId;
};

class MessageProcessor {

  public:
    explicit MessageProcessor(std::shared_ptr<spdlog::logger> logger);
    void processIncomingMessage(const nlohmann::json &envelope, std::string_view message,
                                const WebSocketMessageMetadata &metadata = {});

  private:
    std::shared_ptr<spdlog::logger> logger_;
    std::unordered_map<std::string, std::unique_ptr<creatures::ws::IMessageHandler>> handlers;
};

} // namespace creatures::ws
