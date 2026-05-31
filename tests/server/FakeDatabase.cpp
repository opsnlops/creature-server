#include "server/database.h"

#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

namespace creatures {

namespace {
mongocxx::instance mongoInstance{};

// Test-only toggle. Off by default (every DB call returns a failure stub so
// any accidental DB dependency in a test is loud). Tests that want to
// exercise the success path of the storage publishers set this true, call
// the publisher, and assert on the scheduled-invalidations log.
bool g_pretendSuccess = false;
} // namespace

namespace testing {
void setFakeDatabaseSucceeds(bool v) { g_pretendSuccess = v; }
bool fakeDatabaseSucceeds() { return g_pretendSuccess; }
} // namespace testing

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
    if (g_pretendSuccess)
        return Result<creatures::Creature>{creatures::Creature{}};
    return Result<creatures::Creature>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<std::string> Database::playStoredAnimation(const animationId_t & /*animationId*/, universe_t /*universe*/,
                                                  const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<std::string>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<nlohmann::json> Database::getAnimationJson(const animationId_t & /*animationId*/,
                                                  const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<nlohmann::json>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::Animation> Database::getAnimation(const animationId_t &animationId,
                                                    const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
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
                                                       const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<creatures::Animation>{creatures::Animation{}};
    return Result<creatures::Animation>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::insertAdHocAnimation(const creatures::Animation & /*animation*/,
                                            std::chrono::system_clock::time_point /*createdAt*/,
                                            std::shared_ptr<OperationSpan> /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::deleteAnimation(const animationId_t & /*animationId*/,
                                       const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<nlohmann::json> Database::getCreatureJson(const creatureId_t & /*creatureId*/,
                                                 const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    return Result<nlohmann::json>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

// Stubs for the publisher pattern (issue #11 / PR #21). Each returns a
// failure by default so tests get a clear "this Database method was called
// but not arranged" signal; the testing::setFakeDatabaseSucceeds(true)
// override flips them to success so publishers can be exercised end-to-end.
Result<creatures::DmxFixture> Database::upsertFixture(const std::string & /*fixtureJson*/,
                                                      const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<creatures::DmxFixture>{creatures::DmxFixture{}};
    return Result<creatures::DmxFixture>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::deleteFixture(const fixtureId_t & /*fixtureId*/,
                                     const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::setFixtureUniverse(const fixtureId_t & /*fixtureId*/, std::optional<universe_t> /*universe*/,
                                          const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::Playlist> Database::upsertPlaylist(const std::string & /*playlistJson*/,
                                                     const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<creatures::Playlist>{creatures::Playlist{}};
    return Result<creatures::Playlist>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::DialogScript> Database::upsertDialogScript(const std::string & /*scriptJson*/,
                                                             const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<creatures::DialogScript>{creatures::DialogScript{}};
    return Result<creatures::DialogScript>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::deleteDialogScript(const scriptId_t & /*scriptId*/,
                                          const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<creatures::Storyboard> Database::upsertStoryboard(const std::string & /*storyboardJson*/,
                                                         const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<creatures::Storyboard>{creatures::Storyboard{}};
    return Result<creatures::Storyboard>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

Result<void> Database::deleteStoryboard(const storyboardId_t & /*storyboardId*/,
                                        const std::shared_ptr<OperationSpan> & /*parentSpan*/) {
    if (g_pretendSuccess)
        return Result<void>{};
    return Result<void>{ServerError(ServerError::InvalidData, "FakeDatabase stub")};
}

} // namespace creatures
