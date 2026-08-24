#include "server/ws/dto/AdHocDtoUtils.h"

#include "model/AnimationMetadata.h"
#include "util/helpers.h"

namespace creatures::ws {

oatpp::Object<AdHocAnimationDto> buildAdHocAnimationDto(const creatures::Animation &animation,
                                                        const std::chrono::system_clock::time_point &createdAt) {
    auto dto = AdHocAnimationDto::createShared();
    dto->animation_id = animation.id;
    dto->metadata = creatures::convertToDto(animation.metadata);
    dto->created_at = formatTimeISO8601(createdAt).c_str();
    return dto;
}

} // namespace creatures::ws
