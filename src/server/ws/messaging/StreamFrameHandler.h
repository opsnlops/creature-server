
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/StreamFrame.h"
#include "util/ObservabilityManager.h"

#include "IMessageHandler.h"

namespace creatures::ws {

class StreamFrameHandler : public IMessageHandler {

  public:
    void processMessage(const oatpp::String &payload) override;

  private:
    /**
     * Does the actual work of streaming a frame
     *
     * @param frame the frame to stream
     */
    void stream(StreamFrame frame, std::shared_ptr<SamplingSpan> parentSpan);

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);

    framenum_t framesStreamed = 0;
};

} // namespace creatures::ws
