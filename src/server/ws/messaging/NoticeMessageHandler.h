
#pragma once

#include <memory>
#include <oatpp/core/Types.hpp>

#include "model/Notice.h"

#include "IMessageHandler.h"

namespace creatures::ws {

class NoticeMessageHandler : public IMessageHandler {

  public:
    void processMessage(const oatpp::String &payload) override;
};

} // namespace creatures::ws
