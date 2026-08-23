#include "server/ws/dto/LogItemDto.h"
#include "server/ws/dto/StreamFrameDto.h"
#include "server/ws/dto/VirtualStatusLightsDto.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

oatpp::Object<LogItemDto> convertToDto(const LogItem &logItem) {
    auto dto = LogItemDto::createShared();
    dto->timestamp = logItem.timestamp;
    dto->level = std::string(toString(logItem.level));
    dto->message = logItem.message;
    dto->logger_name = logItem.logger_name;
    dto->thread_id = logItem.thread_id;
    return dto;
}

LogItem convertFromDto(const oatpp::Object<LogItemDto> &dto) {
    return LogItem{dto->timestamp, fromString(*dto->level), dto->message, dto->logger_name, dto->thread_id};
}

oatpp::Object<VirtualStatusLightsDto> convertToDto(const VirtualStatusLights &lights) {
    auto dto = VirtualStatusLightsDto::createShared();
    dto->running = lights.running;
    dto->dmx = lights.dmx;
    dto->streaming = lights.streaming;
    dto->animation_playing = lights.animation_playing;
    return dto;
}

VirtualStatusLights convertFromDto(const oatpp::Object<VirtualStatusLightsDto> &dto) {
    return VirtualStatusLights{dto->running, dto->dmx, dto->streaming, dto->animation_playing};
}

oatpp::Object<StreamFrameDto> convertToDto(const StreamFrame &streamFrame,
                                           const std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability && parentSpan
                    ? observability->createChildOperationSpan("StreamFrame.convertToDto", parentSpan)
                    : nullptr;
    auto dto = StreamFrameDto::createShared();
    dto->creature_id = streamFrame.creature_id;
    dto->universe = streamFrame.universe;
    dto->data = streamFrame.data;
    if (span) {
        span->setAttribute("creature.id", streamFrame.creature_id);
        span->setAttribute("universe", static_cast<int64_t>(streamFrame.universe));
        span->setAttribute("stream_frame.encoded_bytes", static_cast<int64_t>(streamFrame.data.size()));
        span->setSuccess();
    }
    return dto;
}

StreamFrame convertFromDto(const oatpp::Object<StreamFrameDto> &dto, const std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability && parentSpan
                    ? observability->createChildOperationSpan("StreamFrame.convertFromDto", parentSpan)
                    : nullptr;
    const StreamFrame frame{dto->creature_id, dto->universe, dto->data};
    if (span) {
        span->setAttribute("creature.id", frame.creature_id);
        span->setAttribute("universe", static_cast<int64_t>(frame.universe));
        span->setAttribute("stream_frame.encoded_bytes", static_cast<int64_t>(frame.data.size()));
        span->setSuccess();
    }
    return frame;
}

} // namespace creatures
