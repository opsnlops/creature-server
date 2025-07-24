
#pragma once

#include <memory>
#include <oatpp/core/Types.hpp>

namespace creatures ::ws {

class IMessageHandler {
  public:
    virtual ~IMessageHandler() = default;

    virtual void processMessage(const oatpp::String &message) = 0;
};

} // namespace creatures::ws