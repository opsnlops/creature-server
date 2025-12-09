#include "server/database.h"

namespace creatures {

Database::Database(const std::string & /*mongoURI_*/) {}

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

Result<creatures::Animation> Database::getAnimation(const animationId_t & /*animationId*/,
                                                    std::shared_ptr<OperationSpan> /*parentSpan*/) {
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
