
#pragma once

#include "model/StreamFrame.h"
#include "util/ObservabilityManager.h"

#include "IMessageHandler.h"

namespace creatures::ws {

class StreamFrameHandler : public IMessageHandler {

  public:
    using IMessageHandler::IMessageHandler;

    bool processMessage(const nlohmann::json &payload, std::string_view message, std::string_view command,
                        std::shared_ptr<SamplingSpan> messageSpan) override;

  private:
    /**
     * Does the actual work of streaming a frame
     *
     * @param frame the frame to stream
     */
    bool stream(StreamFrame frame, std::shared_ptr<SamplingSpan> parentSpan);

    framenum_t framesStreamed = 0;
};

} // namespace creatures::ws
