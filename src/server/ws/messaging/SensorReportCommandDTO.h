#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

namespace creatures::ws {

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Individual power sensor reading DTO
 */
class PowerSensorReadingDTO : public oatpp::DTO {
    DTO_INIT(PowerSensorReadingDTO, DTO)

    DTO_FIELD_INFO(name) { info->description = "Sensor name (e.g., vbus, motor_power_in, 3v3)"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(voltage) { info->description = "Voltage reading in volts"; }
    DTO_FIELD(Float64, voltage);

    DTO_FIELD_INFO(current) { info->description = "Current reading in amperes"; }
    DTO_FIELD(Float64, current);

    DTO_FIELD_INFO(power) { info->description = "Power reading in watts"; }
    DTO_FIELD(Float64, power);
};

/**
 * Board sensor report payload DTO
 */
class BoardSensorReportPayloadDTO : public oatpp::DTO {
    DTO_INIT(BoardSensorReportPayloadDTO, DTO)

    DTO_FIELD_INFO(creatureId) { info->description = "ID of the creature reporting sensor data"; }
    DTO_FIELD(String, creatureId, "creature_id");

    DTO_FIELD_INFO(creatureName) { info->description = "Name of the creature reporting sensor data"; }
    DTO_FIELD(String, creatureName);

    DTO_FIELD_INFO(board_temperature) { info->description = "Board temperature in Fahrenheit"; }
    DTO_FIELD(Float64, board_temperature);

    DTO_FIELD_INFO(power_reports) { info->description = "Array of power sensor readings"; }
    DTO_FIELD(List<Object<PowerSensorReadingDTO>>, power_reports);
};

/**
 * Board sensor report command DTO wrapper
 */
class BoardSensorReportCommandDTO : public oatpp::DTO {
    DTO_INIT(BoardSensorReportCommandDTO, DTO)

    DTO_FIELD_INFO(payload) { info->description = "Board sensor report payload"; }
    DTO_FIELD(Object<BoardSensorReportPayloadDTO>, payload);
};

#include OATPP_CODEGEN_END(DTO)

} // namespace creatures::ws