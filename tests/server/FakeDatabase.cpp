#include "server/database.h"

#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

namespace creatures {

namespace {
mongocxx::instance mongoInstance{};
}

Database::Database(const std::string &mongoURI_)
    : mongoURI(mongoURI_), mongoPool(mongocxx::uri{mongoURI_.empty() ? "mongodb://localhost:27017" : mongoURI_}) {}

Result<creatures::Creature> Database::getCreature(const creatureId_t &creatureId,
                                                  const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<creatures::Creature>{ServerError(ServerError::NotFound, "FakeDatabase: no creature " + creatureId)};
}

Result<std::vector<creatures::Creature>>
Database::getAllCreatures(creatures::SortBy /*sortBy*/, bool /*ascending*/,
                          const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<std::vector<creatures::Creature>>{std::vector<creatures::Creature>{}};
}

Result<creatures::Creature> Database::upsertCreature(const std::string & /*creatureJson*/,
                                                     const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<std::string> Database::playStoredAnimation(animationId_t /*animationId*/, universe_t /*universe*/,
                                                  std::shared_ptr<OperationSpan> /*parentSpan*/) {
    return Result<std::string>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<nlohmann::json> Database::getAnimationJson(animationId_t /*animationId*/,
                                                  std::shared_ptr<OperationSpan> /*parentSpan*/) {
    return Result<nlohmann::json>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::Animation> Database::getAnimation(const animationId_t &animationId,
                                                    std::shared_ptr<OperationSpan> /*parentSpan*/) {
    if (animationId == "anim-good") {
        creatures::Animation animation;
        animation.id = animationId;
        animation.metadata.animation_id = animationId;
        animation.metadata.title = "Good Animation";
        animation.metadata.milliseconds_per_frame = 20;
        animation.metadata.number_of_frames = 1;
        animation.metadata.multitrack_audio = false;
        creatures::Track track;
        track.id = "track-good";
        track.animation_id = animationId;
        track.creature_id = "creature-123";
        animation.tracks.push_back(track);
        return Result<creatures::Animation>{animation};
    }

    if (animationId == "anim-mismatch") {
        creatures::Animation animation;
        animation.id = animationId;
        animation.metadata.animation_id = animationId;
        animation.metadata.title = "Mismatch Animation";
        animation.metadata.milliseconds_per_frame = 20;
        animation.metadata.number_of_frames = 1;
        animation.metadata.multitrack_audio = false;
        creatures::Track track;
        track.id = "track-mismatch";
        track.animation_id = animationId;
        track.creature_id = "other-creature";
        animation.tracks.push_back(track);
        return Result<creatures::Animation>{animation};
    }

    return Result<creatures::Animation>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<std::vector<creatures::AnimationMetadata>>
Database::listAnimations(creatures::SortBy /*sortBy*/, const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<std::vector<creatures::AnimationMetadata>>{
        ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::Animation> Database::upsertAnimation(const std::string & /*animationJson*/,
                                                       std::shared_ptr<OperationSpan> /*parentSpan*/) {
    return Result<creatures::Animation>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::deleteAnimation(const animationId_t & /*animationId*/,
                                       std::shared_ptr<OperationSpan> /*parentSpan*/) {
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<nlohmann::json> Database::getCreatureJson(creatureId_t /*creatureId*/,
                                                 const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<nlohmann::json>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

} // namespace creatures
