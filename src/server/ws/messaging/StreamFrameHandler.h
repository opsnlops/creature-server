
#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

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
    void stream(StreamFrame frame, std::shared_ptr<OperationSpan> parentSpan);

    OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
    OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper);

    framenum_t framesStreamed = 0;
};

} // namespace creatures::ws
