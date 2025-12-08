
#pragma once

#include "spdlog/spdlog.h"

#include <oatpp/core/macro/component.hpp>
#include <oatpp/web/protocol/http/Http.hpp>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/ws/dto/AdHocAnimationDto.h"
#include "server/ws/dto/ListDto.h"
#include "server/ws/dto/StatusDto.h"
#include "util/ObservabilityManager.h"

namespace creatures ::ws {

class AnimationService {

  private:
    typedef oatpp::web::protocol::http::Status Status;

  public:
    oatpp::Object<ListDto<oatpp::Object<creatures::AnimationMetadataDto>>>
    listAllAnimations(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    oatpp::Object<creatures::AnimationDto> getAnimation(const oatpp::String &animationId,
                                                        std::shared_ptr<RequestSpan> parentSpan = nullptr);
    oatpp::Object<creatures::AnimationDto> getAdHocAnimation(const oatpp::String &animationId,
                                                             std::shared_ptr<RequestSpan> parentSpan = nullptr);
    oatpp::Object<creatures::AnimationDto> upsertAnimation(const std::string &animationJson,
                                                           std::shared_ptr<RequestSpan> parentSpan = nullptr);
    oatpp::Object<AdHocAnimationListDto> listAdHocAnimations(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    oatpp::Object<creatures::ws::StatusDto> deleteAnimation(const oatpp::String &animationId,
                                                            std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Play a single animation on one universe out of the database
     *
     * @param animationId the animation to play
     * @param universe which universe to play the animation in
     * @param reason reason tag for activity reporting (play|playlist|ad_hoc)
     * @return The status of what happened
     */
    oatpp::Object<creatures::ws::StatusDto> playStoredAnimation(const oatpp::String &animationId, universe_t universe,
                                                                const std::string &reason = "play",
                                                                std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
