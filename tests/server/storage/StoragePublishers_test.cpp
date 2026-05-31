// Tests the DB-only publisher half of the storage facade (issue #11
// expansion). Each test:
//   1. Clears the scheduled-invalidation log
//   2. Calls one publisher
//   3. Asserts on the log
//
// The load-bearing property: invalidations fire on success and ONLY on
// success. The failure-stub default of FakeDatabase makes the failure-path
// test trivial; setFakeDatabaseSucceeds(true) flips the stubs to success so
// the happy path can be exercised.

#include <vector>

#include <gtest/gtest.h>

#include "model/CacheInvalidation.h"
#include "server/database.h"
#include "server/storage/Storage.h"

namespace creatures {

extern std::shared_ptr<Database> db;

namespace testing {
void setFakeDatabaseSucceeds(bool v);
const std::vector<CacheType> &scheduledInvalidationsForTesting();
void clearScheduledInvalidationsForTesting();
} // namespace testing

} // namespace creatures

namespace creatures::storage {

class PublishersTest : public ::testing::Test {
  protected:
    void SetUp() override {
        // Each test starts with a real Database instance (FakeDatabase impl)
        // so the `if (!creatures::db)` guard in the publishers passes through
        // to the actual stub. Without this, tests that ran before any test
        // initialized db would all bail on "db unavailable" and never exercise
        // the invalidation path.
        savedDb_ = creatures::db;
        creatures::db = std::make_shared<creatures::Database>(std::string{});

        creatures::testing::clearScheduledInvalidationsForTesting();
        creatures::testing::setFakeDatabaseSucceeds(false);
    }

    void TearDown() override {
        creatures::testing::setFakeDatabaseSucceeds(false);
        creatures::testing::clearScheduledInvalidationsForTesting();
        creatures::db = savedDb_;
    }

    const std::vector<CacheType> &log() { return creatures::testing::scheduledInvalidationsForTesting(); }

    std::shared_ptr<creatures::Database> savedDb_;
};

// =========================================================================
// Success path: invalidations fire on every publisher.
// =========================================================================

TEST_F(PublishersTest, PublishCreatureFiresCreatureOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishCreature("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Creature});
}

TEST_F(PublishersTest, PublishFixtureFiresFixtureOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishFixture("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Fixture});
}

TEST_F(PublishersTest, DeleteFixtureFiresFixtureOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = deleteFixture("some-fixture-id");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Fixture});
}

TEST_F(PublishersTest, SetFixtureUniverseFiresFixtureOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = setFixtureUniverse("some-fixture-id", std::optional<universe_t>{1});
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Fixture});
}

TEST_F(PublishersTest, PublishPlaylistFiresPlaylistOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishPlaylist("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Playlist});
}

TEST_F(PublishersTest, PublishDialogScriptFiresDialogScriptListOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishDialogScript("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::DialogScriptList});
}

TEST_F(PublishersTest, DeleteDialogScriptFiresDialogScriptListOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = deleteDialogScript("some-script-id");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::DialogScriptList});
}

TEST_F(PublishersTest, PublishStoryboardFiresStoryboardListOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishStoryboard("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::StoryboardList});
}

TEST_F(PublishersTest, DeleteStoryboardFiresStoryboardListOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = deleteStoryboard("some-storyboard-id");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::StoryboardList});
}

TEST_F(PublishersTest, DeleteAnimationFiresAnimationOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = deleteAnimation("some-anim-id");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Animation});
}

TEST_F(PublishersTest, PublishAnimationFiresBothAnimationAndSoundListOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishAnimation("{}");
    EXPECT_TRUE(r.isSuccess());
    // Two-invalidation case: order matches the call order in the publisher.
    ASSERT_EQ(log().size(), 2u);
    EXPECT_EQ(log()[0], CacheType::Animation);
    EXPECT_EQ(log()[1], CacheType::SoundList);
}

TEST_F(PublishersTest, RepublishAnimationFiresAnimationOnly) {
    // Lipsync handler's "update without new sound" case — invalidates
    // Animation but NOT SoundList (the sound file reference didn't change).
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = republishAnimation("{}");
    EXPECT_TRUE(r.isSuccess());
    EXPECT_EQ(log(), std::vector{CacheType::Animation});
}

TEST_F(PublishersTest, PublishAdHocAnimationFiresBothAdHocListsOnSuccess) {
    creatures::testing::setFakeDatabaseSucceeds(true);
    auto r = publishAdHocAnimation(creatures::Animation{});
    EXPECT_TRUE(r.isSuccess());
    ASSERT_EQ(log().size(), 2u);
    EXPECT_EQ(log()[0], CacheType::AdHocAnimationList);
    EXPECT_EQ(log()[1], CacheType::AdHocSoundList);
}

// =========================================================================
// Failure path: NO invalidation fires when the DB call returns failure.
// The pairing is atomic — clients should not refresh after a failed publish.
// =========================================================================

TEST_F(PublishersTest, PublishCreatureFiresNothingOnFailure) {
    // setFakeDatabaseSucceeds(false) is the default; explicit for clarity.
    creatures::testing::setFakeDatabaseSucceeds(false);
    auto r = publishCreature("{}");
    EXPECT_FALSE(r.isSuccess());
    EXPECT_TRUE(log().empty());
}

TEST_F(PublishersTest, PublishFixtureFiresNothingOnFailure) {
    auto r = publishFixture("{}");
    EXPECT_FALSE(r.isSuccess());
    EXPECT_TRUE(log().empty());
}

TEST_F(PublishersTest, DeleteDialogScriptFiresNothingOnFailure) {
    auto r = deleteDialogScript("nope");
    EXPECT_FALSE(r.isSuccess());
    EXPECT_TRUE(log().empty());
}

TEST_F(PublishersTest, PublishAnimationFiresNeitherOnFailure) {
    // Two-invalidation case: failure suppresses BOTH, not one-of-two.
    auto r = publishAnimation("{}");
    EXPECT_FALSE(r.isSuccess());
    EXPECT_TRUE(log().empty());
}

// =========================================================================
// Standalone broadcast: deliberately separate from the publisher pattern.
// =========================================================================

TEST_F(PublishersTest, BroadcastCacheInvalidationFiresExactlyOne) {
    broadcastCacheInvalidation(CacheType::Creature);
    EXPECT_EQ(log(), std::vector{CacheType::Creature});
}

TEST_F(PublishersTest, BroadcastDoesNotTouchDb) {
    // Even with the success toggle off, broadcastCacheInvalidation works —
    // it doesn't call into the DB at all. Proves the "manual" case really is
    // independent of the publisher pattern's DB-call check.
    creatures::testing::setFakeDatabaseSucceeds(false);
    broadcastCacheInvalidation(CacheType::Fixture);
    EXPECT_EQ(log(), std::vector{CacheType::Fixture});
}

} // namespace creatures::storage
