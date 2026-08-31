
#pragma once

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/logger.h>

namespace creatures {
class SamplingSpan;
}

namespace creatures::ws {

class IMessageHandler {
  public:
    explicit IMessageHandler(std::shared_ptr<spdlog::logger> logger) : logger_(std::move(logger)) {
        if (!logger_)
            throw std::invalid_argument("IMessageHandler requires a logger");
    }
    virtual ~IMessageHandler() = default;

    virtual bool processMessage(const nlohmann::json &payload, std::string_view message, std::string_view command,
                                std::shared_ptr<SamplingSpan> messageSpan) = 0;

  protected:
    std::shared_ptr<spdlog::logger> logger_;
};

} // namespace creatures::ws
