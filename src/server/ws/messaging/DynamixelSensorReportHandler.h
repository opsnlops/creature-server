
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
 * Handler for "dynamixel-sensor-report" messages.
 *
 * Parses per-servo Dynamixel telemetry (temperature, load, voltage) and stores
 * it in the sensor data cache for metrics export to Honeycomb, then forwards the
 * original message on to connected clients.
 */
class DynamixelSensorReportHandler : public IMessageHandler {

  public:
    void processMessage(const oatpp::String &payload) override;
};

} // namespace creatures::ws
