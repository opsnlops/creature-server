

#include <string>

#include <oatpp/core/Types.hpp>


#include "StreamFrame.h"

namespace creatures {

    StreamFrame convertFromDto(const std::shared_ptr<StreamFrameDto> &streamFrameDto) {
        StreamFrame streamFrame;
        streamFrame.creature_id = streamFrameDto->creature_id;
        streamFrame.universe = streamFrameDto->universe;
        streamFrame.data = streamFrameDto->data;

        return streamFrame;
    }

    // Convert this into its DTO
    oatpp::Object<StreamFrameDto> convertToDto(const StreamFrame &streamFrame) {
        auto streamFrameDto = StreamFrameDto::createShared();
        streamFrameDto->creature_id = streamFrame.creature_id;
        streamFrameDto->universe = streamFrame.universe;
        streamFrameDto->data = streamFrame.data;

        return streamFrameDto;
    }

}