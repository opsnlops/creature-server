#pragma once

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "model/VirtualStatusLights.h"

namespace creatures {

#include OATPP_CODEGEN_BEGIN(DTO)

class VirtualStatusLightsDto : public oatpp::DTO {
    DTO_INIT(VirtualStatusLightsDto, DTO)

    DTO_FIELD_INFO(running) { info->description = "Is the event loop running?"; }
    DTO_FIELD(Boolean, running);
    DTO_FIELD_INFO(dmx) { info->description = "Are we actively sending frames?"; }
    DTO_FIELD(Boolean, dmx);
    DTO_FIELD_INFO(streaming) { info->description = "Is a client streaming to us?"; }
    DTO_FIELD(Boolean, streaming);
    DTO_FIELD_INFO(animation_playing) { info->description = "Is an animation playing?"; }
    DTO_FIELD(Boolean, animation_playing);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<VirtualStatusLightsDto> convertToDto(const VirtualStatusLights &virtualStatusLights);
VirtualStatusLights convertFromDto(const oatpp::Object<VirtualStatusLightsDto> &virtualStatusLightsDto);

} // namespace creatures
