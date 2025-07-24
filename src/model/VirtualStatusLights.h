
#pragma once

#include <string>
#include <vector>

#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "server/namespace-stuffs.h"

namespace creatures {

/**
 * This represents a "virtual view" of the status lights that would normally be
 * on the server's HAT. This is sent to the client so it can make a "virtual"
 * set of status lights if it wishes.
 */
struct VirtualStatusLights {
    bool running;
    bool dmx;
    bool streaming;
    bool animation_playing;
};

#include OATPP_CODEGEN_BEGIN(DTO)

/**
 * Data transfer object for the VirtualStatusLights
 */
class VirtualStatusLightsDto : public oatpp::DTO {

    DTO_INIT(VirtualStatusLightsDto, DTO /* extends */)

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
VirtualStatusLights convertFromDto(const std::shared_ptr<VirtualStatusLightsDto> &virtualStatusLightsDto);

} // namespace creatures