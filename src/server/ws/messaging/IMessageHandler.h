
#pragma once

#include <memory>
#include <string_view>

#include <nlohmann/json.hpp>

namespace creatures {
class SamplingSpan;
}

namespace creatures::ws {

class IMessageHandler {
  public:
    virtual ~IMessageHandler() = default;

    virtual bool processMessage(const nlohmann::json &payload, std::string_view message, std::string_view command,
                                std::shared_ptr<SamplingSpan> messageSpan) = 0;
};

} // namespace creatures::ws
