
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/core/macro/component.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/ObservabilityManager.h"

namespace creatures :: ws {

    class AnimationService {

    private:
        typedef oatpp::web::protocol::http::Status Status;

    public:

        oatpp::Object<ListDto<oatpp::Object<creatures::AnimationMetadataDto>>> listAllAnimations(std::shared_ptr<RequestSpan> parentSpan = nullptr);
        oatpp::Object<creatures::AnimationDto> getAnimation(const oatpp::String& animationId, std::shared_ptr<RequestSpan> parentSpan = nullptr);
        oatpp::Object<creatures::AnimationDto> upsertAnimation(const std::string& animationJson);


        /**
         * Play a single animation on one universe out of the database
         *
         * @param animationId the animation to play
         * @param universe which universe to play the animation in
         * @return The status of what happened
         */
        oatpp::Object<creatures::ws::StatusDto> playStoredAnimation(const oatpp::String& animationId, universe_t universe);
    };


} // creatures :: ws
