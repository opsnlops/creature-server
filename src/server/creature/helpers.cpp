#include <string>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "exception/exception.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"

namespace creatures {

extern std::shared_ptr<ObservabilityManager> observability;

Result<Creature> Database::creatureFromJson(json creatureJson, std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability->createChildOperationSpan("Database.creatureFromJson", parentSpan);
    const auto result = creatures::creatureFromJson(creatureJson);
    if (!result.isSuccess()) {
        const auto error = result.getError().value();
        if (span) {
            span->setError(error.getMessage());
            span->setAttribute("error.type", "InvalidData");
            span->setAttribute("error.code", static_cast<int64_t>(error.getCode()));
            span->setAttribute("error.message", error.getMessage());
        }
        warn("Creature configuration rejected: {}", error.getMessage());
        return Result<Creature>{error};
    }

    const auto creature = result.getValue().value();
    if (span) {
        span->setAttribute("creature.id", creature.id);
        span->setAttribute("creature.name", creature.name);
        span->setAttribute("creature.audio_channel", static_cast<int64_t>(creature.audio_channel));
        span->setAttribute("creature.channel_offset", static_cast<int64_t>(creature.channel_offset));
        span->setAttribute("creature.mouth_slot", static_cast<int64_t>(creature.mouth_slot));
        span->setAttribute("creature.inputs.count", static_cast<int64_t>(creature.inputs.size()));
        span->setAttribute("creature.idle_animation_ids.count",
                           static_cast<int64_t>(creature.idle_animation_ids.size()));
        span->setAttribute("creature.speech_loop_animation_ids.count",
                           static_cast<int64_t>(creature.speech_loop_animation_ids.size()));
        span->setSuccess();
    }
    return Result<Creature>{creature};
}

Result<Creature> Database::parseCreatureJson(json creatureJson, std::shared_ptr<OperationSpan> parentSpan) {
    return creatureFromJson(std::move(creatureJson), std::move(parentSpan));
}

Result<Creature> Database::creatureFromStoredJson(json creatureJson, std::shared_ptr<OperationSpan> parentSpan) {
    if (creatureJson.is_object())
        creatureJson.erase("_id");
    return creatureFromJson(std::move(creatureJson), std::move(parentSpan));
}

Result<bool> Database::has_required_fields(const nlohmann::json &json, const std::vector<std::string> &requiredFields) {
    if (!json.is_object())
        return Result<bool>{ServerError(ServerError::InvalidData, "Expected a JSON object")};
    for (const auto &field : requiredFields) {
        if (!json.contains(field)) {
            return Result<bool>{
                ServerError(ServerError::InvalidData, fmt::format("Missing required field '{}'", field))};
        }
    }
    return Result<bool>{true};
}

Result<bool> Database::validateCreatureJson(const nlohmann::json &json) {
    const auto result = creatures::creatureFromJson(json);
    if (!result.isSuccess())
        return Result<bool>{result.getError().value()};
    return Result<bool>{true};
}

Result<bool> Database::validateAnimationJson(const nlohmann::json &json) {
    const auto animation = creatures::animationFromJson(json, AnimationJsonSource::Api);
    if (!animation.isSuccess())
        return Result<bool>{animation.getError().value()};
    return Result<bool>{true};
}

Result<bool> Database::validatePlaylistJson(const nlohmann::json &json) {
    const auto result = creatures::playlistFromJson(json);
    if (!result.isSuccess())
        return Result<bool>{result.getError().value()};
    return Result<bool>{true};
}

} // namespace creatures
