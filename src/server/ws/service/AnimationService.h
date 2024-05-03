
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures :: ws {

    class AnimationService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        oatpp::Object<ListDto<oatpp::Object<creatures::AnimationMetadataDto>>> listAllAnimations();
        oatpp::Object<creatures::AnimationDto> getAnimation(const oatpp::String& animationId);
        oatpp::String createAnimation(const oatpp::Object<creatures::AnimationDto>& inAnimationDto);

    };


} // creatures :: ws
