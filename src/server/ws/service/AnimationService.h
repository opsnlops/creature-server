
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/ws/dto/PageDto.h"
#include "server/ws/dto/StatusDto.h"

namespace creatures :: ws {

    class AnimationService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        oatpp::Object<PageDto<oatpp::Object<creatures::AnimationMetadataDto>>> listAllAnimations();
        oatpp::Object<creatures::AnimationDto> getAnimation(const oatpp::String& animationId);

    };


} // creatures :: ws
