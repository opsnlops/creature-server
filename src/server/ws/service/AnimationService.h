#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "model/Animation.h"
#include "model/AnimationMetadata.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

struct AdHocAnimationSummary {
    std::string animationId;
    AnimationMetadata metadata;
    std::string createdAt;
};

struct PlayAnimationResult {
    std::string message;
    std::optional<std::string> sessionId;
};

/// Framework-neutral Animation application service. HTTP status selection and
/// transport serialization belong to the controller adapter.
class AnimationService {
  public:
    Result<std::vector<AnimationMetadata>> listAllAnimations(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<std::vector<AdHocAnimationSummary>> listAdHocAnimations(std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<Animation> getAnimation(const std::string &animationId, std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<Animation> getAdHocAnimation(const std::string &animationId,
                                        std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<Animation> upsertAnimation(const std::string &animationJson,
                                      std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<void> deleteAnimation(const std::string &animationId, std::shared_ptr<RequestSpan> parentSpan = nullptr);
    Result<PlayAnimationResult> playStoredAnimation(const std::string &animationId, universe_t universe,
                                                    const std::string &reason = "play",
                                                    std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
