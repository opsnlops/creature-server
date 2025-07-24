
#pragma once

#include <oatpp/core/Types.hpp>

#include "IMessageHandler.h"

namespace creatures::ws {

/**
 * Handler for sensor report messages.
 *
 * This handles both motor and board sensor reports.
 */
class SensorReportHandler : public IMessageHandler {

  public:
    void processMessage(const oatpp::String &payload) override;
};

} // namespace creatures::ws
