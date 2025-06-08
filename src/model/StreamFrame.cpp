

#include <string>

#include <oatpp/core/Types.hpp>

#include "util/ObservabilityManager.h"

#include "StreamFrame.h"

namespace creatures {
    extern std::shared_ptr<ObservabilityManager> observability;
}


namespace creatures {

    StreamFrame convertFromDto(const std::shared_ptr<StreamFrameDto> &streamFrameDto, std::shared_ptr<OperationSpan> parentSpan) {

        // Only record a span if we have a parent span
        std::shared_ptr<OperationSpan> span = nullptr;
        if(parentSpan) {
            span = creatures::observability->createChildOperationSpan("StreamFrame.convertFromDto", parentSpan);
        }

        StreamFrame streamFrame;
        streamFrame.creature_id = streamFrameDto->creature_id;
        streamFrame.universe = streamFrameDto->universe;
        streamFrame.data = streamFrameDto->data;

        if(span) {
            span->setSuccess();
            span->setAttribute("creature_id", streamFrame.creature_id);
            span->setAttribute("universe", static_cast<int64_t>(streamFrame.universe));
            span->setAttribute("data_size", static_cast<int64_t>(streamFrame.data.size()));
        }

        return streamFrame;
    }

    // Convert this into its DTO
    oatpp::Object<StreamFrameDto> convertToDto(const StreamFrame &streamFrame, std::shared_ptr<OperationSpan> parentSpan) {

        // Only record a span if we have a parent span
        std::shared_ptr<OperationSpan> span = nullptr;
        if(parentSpan) {
            span = creatures::observability->createChildOperationSpan("StreamFrame.convertToDto", parentSpan);
        }

        auto streamFrameDto = StreamFrameDto::createShared();
        streamFrameDto->creature_id = streamFrame.creature_id;
        streamFrameDto->universe = streamFrame.universe;
        streamFrameDto->data = streamFrame.data;

        if(span) {
            span->setSuccess();
            span->setAttribute("creature_id", streamFrame.creature_id);
            span->setAttribute("universe", static_cast<int64_t>(streamFrame.universe));
            span->setAttribute("data_size", static_cast<int64_t>(streamFrame.data.size()));
        }

        return streamFrameDto;
    }

}