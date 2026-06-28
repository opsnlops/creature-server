#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Individual Dynamixel servo telemetry reading DTO
 *
 * Mirrors the per-motor objects the controller emits in the
 * "dynamixel-sensor-report" message (one entry per servo on the bus).
 */
class DynamixelSensorReadingDTO : public oatpp::DTO {
    DTO_INIT(DynamixelSensorReadingDTO, DTO)

    DTO_FIELD_INFO(dxl_id) { info->description = "Dynamixel servo ID on the bus"; }
    DTO_FIELD(Int32, dxl_id);

    DTO_FIELD_INFO(temperature_f) { info->description = "Servo temperature in Fahrenheit"; }
    DTO_FIELD(Float64, temperature_f);

    DTO_FIELD_INFO(present_load) { info->description = "Present load (raw, signed; negative = opposing direction)"; }
    DTO_FIELD(Int32, present_load);

    DTO_FIELD_INFO(voltage_mv) { info->description = "Input voltage in millivolts"; }
    DTO_FIELD(Int32, voltage_mv);

    DTO_FIELD_INFO(voltage_v) { info->description = "Input voltage in volts"; }
    DTO_FIELD(Float64, voltage_v);
};

/**
 * Dynamixel sensor report payload DTO
 */
class DynamixelSensorReportPayloadDTO : public oatpp::DTO {
    DTO_INIT(DynamixelSensorReportPayloadDTO, DTO)

    DTO_FIELD_INFO(creatureId) { info->description = "ID of the creature reporting sensor data"; }
    DTO_FIELD(String, creatureId, "creature_id");

    DTO_FIELD_INFO(creatureName) { info->description = "Name of the creature reporting sensor data"; }
    DTO_FIELD(String, creatureName);

    DTO_FIELD_INFO(dynamixel_motors) { info->description = "Array of per-servo Dynamixel telemetry readings"; }
    DTO_FIELD(List<Object<DynamixelSensorReadingDTO>>, dynamixel_motors);
};

/**
 * Dynamixel sensor report command DTO wrapper
 */
class DynamixelSensorReportCommandDTO : public oatpp::DTO {
    DTO_INIT(DynamixelSensorReportCommandDTO, DTO)

    DTO_FIELD_INFO(payload) { info->description = "Dynamixel sensor report payload"; }
    DTO_FIELD(Object<DynamixelSensorReportPayloadDTO>, payload);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws
