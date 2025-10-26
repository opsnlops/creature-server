#pragma once

#include <chrono>

#include <oatpp/core/Types.hpp>

#include "model/Animation.h"

#include "server/ws/dto/AdHocAnimationDto.h"
#include "server/ws/dto/AdHocSoundEntryDto.h"

namespace creatures::ws {

oatpp::Object<AdHocSoundEntryDto> buildAdHocSoundEntryDto(const creatures::Animation &animation,
                                                          const std::chrono::system_clock::time_point &createdAt);

oatpp::Object<AdHocAnimationDto> buildAdHocAnimationDto(const creatures::Animation &animation,
                                                        const std::chrono::system_clock::time_point &createdAt);

} // namespace creatures::ws

