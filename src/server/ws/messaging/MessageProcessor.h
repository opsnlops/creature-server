
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <oatpp/core/Types.hpp>

#include "IMessageHandler.h"

namespace creatures ::ws {

class MessageProcessor {

  public:
    MessageProcessor();
    void processIncomingMessage(const nlohmann::json &envelope, const oatpp::String &message);

  private:
    std::unordered_map<std::string, std::unique_ptr<creatures::ws::IMessageHandler>> handlers;
};

} // namespace creatures::ws
