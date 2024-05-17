
#pragma once


#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/component.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/StreamFrame.h"

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
        void stream(StreamFrame frame);

        OATPP_COMPONENT(std::shared_ptr<spdlog::logger>, appLogger);
        OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, apiObjectMapper);

        uint64_t framesStreamed = 0;
    };

} //  creatures: ws
