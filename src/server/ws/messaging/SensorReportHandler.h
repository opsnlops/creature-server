
#pragma once

#include <memory>
#include <oatpp/core/Types.hpp>

#include "IMessageHandler.h"

namespace creatures {
class SensorDataCache;
class ObservabilityManager;
} // namespace creatures

namespace creatures::ws {

/**
 * Handler for sensor report messages.
 *
 * This handles both motor and board sensor reports.
 * Parses sensor data and stores it in a cache for metrics export to Honeycomb.
 */
class SensorReportHandler : public IMessageHandler {

  public:
    void processMessage(const oatpp::String &payload) override;
};

} // namespace creatures::ws
