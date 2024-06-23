
#pragma once

#include <oatpp/core/Types.hpp>

#include "IMessageHandler.h"


namespace creatures::ws {

    class SensorReportHandler : public IMessageHandler {

    public:
        void processMessage(const oatpp::String &payload) override;
    };

} //  creatures: ws
