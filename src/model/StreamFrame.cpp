

#include <vector>
#include <string>

#include <oatpp/core/Types.hpp>


#include "StreamFrame.h"

namespace creatures {

    StreamFrame convertFromDto(const std::shared_ptr<StreamFrameDto> &streamFrameDto) {
        StreamFrame streamFrame;
        streamFrame.creature_id = streamFrameDto->creature_id;
        streamFrame.data = streamFrameDto->data;

        return streamFrame;
    }

    // Convert FrameData to FrameDataDTO
    std::shared_ptr<StreamFrameDto> convertToDto(const StreamFrame &streamFrame) {
        auto streamFrameDto = StreamFrameDto::createShared();
        streamFrameDto->creature_id = streamFrame.creature_id;
        streamFrameDto->data = streamFrame.data;

        return streamFrameDto.getPtr();
    }

}