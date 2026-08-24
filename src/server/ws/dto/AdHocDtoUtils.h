#pragma once

#include <chrono>

#include <oatpp/core/Types.hpp>

#include "model/Animation.h"

#include "server/ws/dto/AdHocAnimationDto.h"

namespace creatures::ws {

oatpp::Object<AdHocAnimationDto> buildAdHocAnimationDto(const creatures::Animation &animation,
                                                        const std::chrono::system_clock::time_point &createdAt);

} // namespace creatures::ws
