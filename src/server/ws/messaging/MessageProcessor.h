
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

#include "IMessageHandler.h"

namespace creatures ::ws {

class MessageProcessor {

  public:
    explicit MessageProcessor(std::shared_ptr<spdlog::logger> logger);
    void processIncomingMessage(const nlohmann::json &envelope, std::string_view message);

  private:
    std::shared_ptr<spdlog::logger> logger_;
    std::unordered_map<std::string, std::unique_ptr<creatures::ws::IMessageHandler>> handlers;
};

} // namespace creatures::ws
