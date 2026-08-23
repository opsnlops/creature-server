#include "server/ws/dto/AdHocExchangeDto.h"

#include "util/helpers.h"

namespace creatures {

oatpp::Object<AdHocExchangeDto> convertToDto(const AdHocExchange &exchange,
                                             const std::chrono::system_clock::time_point &createdAt) {
    auto dto = AdHocExchangeDto::createShared();
    dto->session_id = exchange.session_id;
    dto->creature_id = exchange.creature_id;
    dto->creature_name = exchange.creature_name;
    dto->status = exchange.status;
    dto->title = exchange.title;
    dto->transcript = exchange.transcript;
    dto->duration_ms = exchange.duration_ms;
    dto->created_at = formatTimeISO8601(createdAt);
    if (exchange.finished_at_ms > 0) {
        dto->finished_at = formatTimeISO8601(
            std::chrono::system_clock::time_point(std::chrono::milliseconds(exchange.finished_at_ms)));
    }
    dto->parts = oatpp::Vector<oatpp::Object<AdHocExchangePartDto>>::createShared();
    for (const auto &part : exchange.parts) {
        auto partDto = AdHocExchangePartDto::createShared();
        partDto->index = part.index;
        partDto->animation_id = part.animation_id;
        partDto->text = part.text;
        partDto->duration_ms = part.duration_ms;
        dto->parts->push_back(partDto);
    }
    return dto;
}

} // namespace creatures
