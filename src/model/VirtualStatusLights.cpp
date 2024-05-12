

#include <string>

#include <oatpp/core/Types.hpp>

#include "VirtualStatusLights.h"

namespace creatures {

    VirtualStatusLights convertFromDto(const std::shared_ptr<VirtualStatusLightsDto> &virtualStatusLightsDto) {
        VirtualStatusLights virtualStatusLights;
        virtualStatusLights.running = virtualStatusLightsDto->running;
        virtualStatusLights.dmx = virtualStatusLightsDto->dmx;
        virtualStatusLights.streaming = virtualStatusLightsDto->streaming;

        return virtualStatusLights;
    }

    // Convert this into its DTO
    oatpp::Object<VirtualStatusLightsDto> convertToDto(const VirtualStatusLights &virtualStatusLights) {
        auto virtualStatusLightsDto = VirtualStatusLightsDto::createShared();
        virtualStatusLightsDto->running = virtualStatusLights.running;
        virtualStatusLightsDto->dmx = virtualStatusLights.dmx;
        virtualStatusLightsDto->streaming = virtualStatusLights.streaming;

        return virtualStatusLightsDto;
    }

}