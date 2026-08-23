
#pragma once

#include "model/Notice.h"

#include "IMessageHandler.h"

namespace creatures::ws {

class NoticeMessageHandler : public IMessageHandler {

  public:
    bool processMessage(const nlohmann::json &payload, const oatpp::String &message, std::string_view command,
                        std::shared_ptr<SamplingSpan> messageSpan) override;
};

} // namespace creatures::ws
