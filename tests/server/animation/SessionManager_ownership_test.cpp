// Owner-aware SessionManager cleanup (issue #100).
//
// Non-conflicting sessions (disjoint creature sets) coexist on one universe,
// but the chained animation queue and playlist state used to be stored with no
// owner — so one session's async load failure could wipe state that belonged
// to a different, still-live session, and a failed interrupter stranded a
// playlist in Interrupted forever. These tests pin the ownership rules.

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "../TestGlobals.h"
#include "model/Animation.h"
#include "server/animation/CooperativeAnimationScheduler.h"
#include "server/animation/PlaybackSession.h"
#include "server/animation/SessionManager.h"
#include "server/eventloop/eventloop.h"

namespace creatures {

namespace {

Animation makeAnimation(const std::string &id, const std::string &creatureId) {
    Animation animation;
    animation.id = id;
    animation.metadata.title = "test " + id;
    Track track;
    track.creature_id = creatureId;
    animation.tracks.push_back(track);
    return animation;
}

std::shared_ptr<PlaybackSession>
makeSession(const std::string &animationId, const std::string &creatureId, const std::string &chainId = {},
            creatures::runtime::ActivityReason reason = creatures::runtime::ActivityReason::AdHoc) {
    auto session =
        std::make_shared<PlaybackSession>(makeAnimation(animationId, creatureId), /*universe=*/1, /*startingFrame=*/0,
                                          /*parentSpan=*/nullptr);
    session->setChainId(chainId);
    session->setActivityReason(reason);
    return session;
}

std::shared_ptr<PlaybackSession> makePlaylistSession(const std::string &animationId, const std::string &creatureId) {
    return makeSession(animationId, creatureId, {}, creatures::runtime::ActivityReason::Playlist);
}

constexpr universe_t UNIVERSE = 1;

} // namespace

TEST(SessionManagerOwnership, PlaylistGenerationsInvalidateSupersededAndStoppedEvents) {
    SessionManager manager;

    const auto first = manager.startPlaylist(UNIVERSE, "playlist-1");
    ASSERT_TRUE(manager.claimPlaylistEvent(UNIVERSE));

    const auto second = manager.startPlaylist(UNIVERSE, "playlist-2");
    EXPECT_GT(second.generation, first.generation);
    const auto replacement = manager.beginPlaylistEvent(UNIVERSE, first.generation);
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->generation, second.generation);

    manager.stopPlaylist(UNIVERSE);
    EXPECT_FALSE(manager.beginPlaylistEvent(UNIVERSE, second.generation));
}

TEST(SessionManagerOwnership, RecreatedPlaylistDoesNotDuplicateItsAlreadyClaimedEvent) {
    SessionManager manager;

    const auto first = manager.startPlaylist(UNIVERSE, "playlist-1");
    ASSERT_TRUE(manager.claimPlaylistEvent(UNIVERSE));
    manager.clearPlaylist(UNIVERSE);

    const auto second = manager.startPlaylist(UNIVERSE, "playlist-2");
    ASSERT_TRUE(manager.claimPlaylistEvent(UNIVERSE));

    EXPECT_FALSE(manager.beginPlaylistEvent(UNIVERSE, first.generation));
    const auto current = manager.beginPlaylistEvent(UNIVERSE, second.generation);
    ASSERT_TRUE(current);
    EXPECT_EQ(current->generation, second.generation);
}

TEST(SessionManagerOwnership, NonConflictingSessionsCoexistWithAscendingGenerations) {
    SessionManager manager;
    auto sessionA = makeSession("anim-a", "creature-a");
    auto sessionB = makeSession("anim-b", "creature-b");

    manager.registerSession(UNIVERSE, sessionA);
    manager.registerSession(UNIVERSE, sessionB);

    const auto active = manager.getActiveSessions(UNIVERSE);
    ASSERT_EQ(active.size(), 2U);
    EXPECT_FALSE(sessionA->isCancelled());
    EXPECT_FALSE(sessionB->isCancelled());
    EXPECT_LT(sessionA->getActivityGeneration(), sessionB->getActivityGeneration());
}

TEST(SessionManagerOwnership, AbortOfBystanderCannotDropAnotherChainsQueue) {
    SessionManager manager;
    auto chainSession = makeSession("anim-a", "creature-a", "chain-1");
    auto bystander = makeSession("anim-b", "creature-b");

    manager.registerSession(UNIVERSE, chainSession);
    manager.registerSession(UNIVERSE, bystander);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));
    ASSERT_TRUE(manager.hasQueuedAnimation(UNIVERSE));

    const auto result = manager.abortLoadingSession(UNIVERSE, bystander->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_EQ(result.queuedAnimationsDropped, 0U);
    EXPECT_TRUE(manager.hasQueuedAnimation(UNIVERSE)) << "chain-1's entry must survive the bystander's failure";
    EXPECT_FALSE(result.playlistCleared);
}

TEST(SessionManagerOwnership, AbortOfChainSessionDropsExactlyItsOwnEntries) {
    SessionManager manager;
    auto chainOne = makeSession("anim-a", "creature-a", "chain-1");
    auto chainTwo = makeSession("anim-b", "creature-b", "chain-2");

    manager.registerSession(UNIVERSE, chainOne);
    manager.registerSession(UNIVERSE, chainTwo);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-b2", "creature-b"), "chain-2"));

    const auto result = manager.abortLoadingSession(UNIVERSE, chainOne->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_EQ(result.queuedAnimationsDropped, 1U);
    // chain-2's entry survives and remains poppable by its owner.
    EXPECT_TRUE(manager.hasQueuedAnimation(UNIVERSE));
    EXPECT_TRUE(manager.popQueuedAnimation(UNIVERSE, "chain-2").has_value());
}

TEST(SessionManagerOwnership, OnlyTheOwningChainCanPop) {
    SessionManager manager;
    auto chainSession = makeSession("anim-a", "creature-a", "chain-1");
    manager.registerSession(UNIVERSE, chainSession);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));

    EXPECT_FALSE(manager.popQueuedAnimation(UNIVERSE, "").has_value()) << "chainless sessions pop nothing";
    EXPECT_FALSE(manager.popQueuedAnimation(UNIVERSE, "chain-2").has_value()) << "other chains pop nothing";
    auto popped = manager.popQueuedAnimation(UNIVERSE, "chain-1");
    ASSERT_TRUE(popped.has_value());
    EXPECT_EQ(popped->id, "anim-a2");
}

TEST(SessionManagerOwnership, EnqueueForDeadChainIsRefused) {
    SessionManager manager;
    auto other = makeSession("anim-b", "creature-b");
    manager.registerSession(UNIVERSE, other);

    // No live session carries chain-1: the entry could never be popped. The
    // caller sees the refusal and can schedule the animation directly instead.
    EXPECT_FALSE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));
    EXPECT_FALSE(manager.hasQueuedAnimation(UNIVERSE));

    // Ownerless entries are refused outright.
    EXPECT_FALSE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a3", "creature-a"), ""));
    EXPECT_FALSE(manager.hasQueuedAnimation(UNIVERSE));
}

TEST(SessionManagerOwnership, PopSkipsForeignEntriesInsteadOfBlocking) {
    SessionManager manager;
    auto chainOne = makeSession("anim-a", "creature-a", "chain-1");
    auto chainTwo = makeSession("anim-b", "creature-b", "chain-2");
    manager.registerSession(UNIVERSE, chainOne);
    manager.registerSession(UNIVERSE, chainTwo);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-b2", "creature-b"), "chain-2"));

    // chain-2's entry sits behind chain-1's, but must still be reachable.
    auto poppedB = manager.popQueuedAnimation(UNIVERSE, "chain-2");
    ASSERT_TRUE(poppedB.has_value());
    EXPECT_EQ(poppedB->id, "anim-b2");
    auto poppedA = manager.popQueuedAnimation(UNIVERSE, "chain-1");
    ASSERT_TRUE(poppedA.has_value());
    EXPECT_EQ(poppedA->id, "anim-a2");
}

TEST(SessionManagerOwnership, CancelledChainEntriesArePurgedOnAdoption) {
    SessionManager manager;
    auto chainSession = makeSession("anim-a", "creature-a", "chain-1");
    manager.registerSession(UNIVERSE, chainSession);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));

    // A conflicting adoption (same creature, different chain) kills chain-1;
    // its onFinish will never pop, so its entries must be purged with it.
    auto replacement = makeSession("anim-x", "creature-a");
    manager.registerSession(UNIVERSE, replacement);

    EXPECT_TRUE(chainSession->isCancelled());
    EXPECT_FALSE(manager.hasQueuedAnimation(UNIVERSE)) << "a dead chain's entries must not linger";
}

TEST(SessionManagerOwnership, SameChainReplacementKeepsEntries) {
    SessionManager manager;
    auto sentenceOne = makeSession("anim-a", "creature-a", "chain-1");
    manager.registerSession(UNIVERSE, sentenceOne);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a3", "creature-a"), "chain-1"));

    // The next hop of the same chain conflicts (same creature) and replaces
    // sentence one — the chain continues, so its queue must survive.
    auto sentenceTwo = makeSession("anim-a2", "creature-a", "chain-1");
    manager.registerSession(UNIVERSE, sentenceTwo);

    EXPECT_TRUE(manager.hasQueuedAnimation(UNIVERSE));
    EXPECT_TRUE(manager.popQueuedAnimation(UNIVERSE, "chain-1").has_value());
}

TEST(SessionManagerOwnership, DropChainEntriesDropsExactlyThatChain) {
    SessionManager manager;
    auto chainOne = makeSession("anim-a", "creature-a", "chain-1");
    auto chainTwo = makeSession("anim-b", "creature-b", "chain-2");
    manager.registerSession(UNIVERSE, chainOne);
    manager.registerSession(UNIVERSE, chainTwo);
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-a2", "creature-a"), "chain-1"));
    EXPECT_TRUE(manager.queueAnimation(UNIVERSE, makeAnimation("anim-b2", "creature-b"), "chain-2"));

    manager.dropChainEntries(UNIVERSE, "chain-1");

    EXPECT_FALSE(manager.popQueuedAnimation(UNIVERSE, "chain-1").has_value());
    EXPECT_TRUE(manager.popQueuedAnimation(UNIVERSE, "chain-2").has_value());
}

TEST(SessionManagerOwnership, PlaylistSurvivesFailureOfNonOwnerPlaylistSession) {
    SessionManager manager;
    manager.startPlaylist(UNIVERSE, "playlist-1");

    auto staleOwner = makePlaylistSession("anim-a", "creature-a");
    manager.registerSession(UNIVERSE, staleOwner, /*isPlaylist=*/true);

    // A newer playlist session takes ownership (disjoint creatures, so the
    // stale one is not cancelled by conflict — it just isn't the owner).
    auto currentOwner = makePlaylistSession("anim-b", "creature-b");
    manager.registerSession(UNIVERSE, currentOwner, /*isPlaylist=*/true);

    const auto result = manager.abortLoadingSession(UNIVERSE, staleOwner->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_FALSE(result.playlistCleared) << "only the exact current owner may clear playlist state";
    EXPECT_EQ(manager.getPlaylistState(UNIVERSE), PlaylistState::Active);
    ASSERT_TRUE(manager.getPlaylistStatus(UNIVERSE).has_value());
    EXPECT_EQ(manager.getPlaylistStatus(UNIVERSE)->playlist, "playlist-1");
}

TEST(SessionManagerOwnership, ExactPlaylistOwnerFailureClearsPlaylist) {
    SessionManager manager;
    manager.startPlaylist(UNIVERSE, "playlist-1");

    auto owner = makePlaylistSession("anim-a", "creature-a");
    manager.registerSession(UNIVERSE, owner, /*isPlaylist=*/true);

    const auto result = manager.abortLoadingSession(UNIVERSE, owner->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_TRUE(result.playlistCleared);
    EXPECT_EQ(manager.getPlaylistState(UNIVERSE), PlaylistState::None);
    EXPECT_FALSE(manager.getPlaylistStatus(UNIVERSE).has_value());
}

TEST(SessionManagerOwnership, InterruptedPlaylistSurvivesBystanderFailure) {
    SessionManager manager;
    manager.startPlaylist(UNIVERSE, "playlist-1");

    // Simulate an interrupt owned by someone else by putting the playlist into
    // Interrupted via a bystander abort check: register two sessions, only one
    // of which will fail. The playlist decision must be untouched by the
    // non-owner's failure.
    auto playlistSession = makePlaylistSession("anim-p", "creature-p");
    manager.registerSession(UNIVERSE, playlistSession, /*isPlaylist=*/true);

    auto bystander = makeSession("anim-b", "creature-b");
    manager.registerSession(UNIVERSE, bystander);

    const auto result = manager.abortLoadingSession(UNIVERSE, bystander->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_FALSE(result.playlistCleared);
    EXPECT_FALSE(result.playlistResumed);
    EXPECT_FALSE(result.playlistStopped);
    EXPECT_EQ(manager.getPlaylistState(UNIVERSE), PlaylistState::Active);
}

// Fixture for tests that drive SessionManager::interrupt(): the fake
// scheduleAnimation (FakeIdleScheduling.cpp) registers sessions through the
// global sessionManager, and interrupt() requires a non-null eventLoop.
class SessionManagerInterruptTest : public ::testing::Test {
  protected:
    void SetUp() override {
        manager_ = std::make_shared<SessionManager>();
        sessionManager = manager_;
        eventLoop = std::make_shared<EventLoop>(); // inert stub; never started
    }

    void TearDown() override {
        sessionManager.reset();
        eventLoop.reset();
    }

    std::shared_ptr<PlaybackSession> startInterrupt(bool shouldResume, const std::string &chainId = {}) {
        manager_->startPlaylist(UNIVERSE, "playlist-1");
        auto playlistSession = makePlaylistSession("anim-p", "creature-p");
        manager_->registerSession(UNIVERSE, playlistSession, /*isPlaylist=*/true);

        auto result =
            manager_->interrupt(UNIVERSE, makeAnimation("anim-i", "creature-i"), shouldResume, nullptr, chainId);
        EXPECT_TRUE(result.isSuccess());
        EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Interrupted);
        return result.getValue().value();
    }

    std::shared_ptr<SessionManager> manager_;
};

TEST_F(SessionManagerInterruptTest, FailedInterrupterResumesPlaylistPerStoredDecision) {
    auto interrupter = startInterrupt(/*shouldResume=*/true);

    const auto result = manager_->abortLoadingSession(UNIVERSE, interrupter->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_TRUE(result.playlistResumed);
    EXPECT_FALSE(result.playlistStopped);
    EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Active)
        << "a failed interrupter must not strand the playlist Interrupted";
    ASSERT_TRUE(manager_->getPlaylistStatus(UNIVERSE).has_value());
    EXPECT_TRUE(manager_->getPlaylistStatus(UNIVERSE)->playing);
}

TEST_F(SessionManagerInterruptTest, FailedInterrupterStopsPlaylistWhenResumeDeclined) {
    auto interrupter = startInterrupt(/*shouldResume=*/false);

    const auto result = manager_->abortLoadingSession(UNIVERSE, interrupter->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_FALSE(result.playlistResumed);
    EXPECT_TRUE(result.playlistStopped);
    EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Stopped);
    ASSERT_TRUE(manager_->getPlaylistStatus(UNIVERSE).has_value());
    EXPECT_FALSE(manager_->getPlaylistStatus(UNIVERSE)->playing);
}

TEST_F(SessionManagerInterruptTest, FailedChainSentenceStillResolvesTheInterrupt) {
    // Sentence one interrupts as chain-9 and queues sentence two.
    auto sentenceOne = startInterrupt(/*shouldResume=*/true, "chain-9");
    EXPECT_TRUE(manager_->queueAnimation(UNIVERSE, makeAnimation("anim-i2", "creature-i"), "chain-9"));

    // Sentence one finishes; its onFinish pops the chain entry and schedules
    // sentence two with the inherited chain id (mirrored here via the fake
    // scheduler), then clears itself.
    auto popped = manager_->popQueuedAnimation(UNIVERSE, "chain-9");
    ASSERT_TRUE(popped.has_value());
    auto sentenceTwoResult = CooperativeAnimationScheduler::scheduleAnimation(
        0, *popped, UNIVERSE, creatures::runtime::ActivityReason::AdHoc, false, "chain-9");
    ASSERT_TRUE(sentenceTwoResult.isSuccess());
    manager_->clearSession(UNIVERSE, sentenceOne->getSessionId());

    // Sentence two's async load fails: the chain owns the interrupt, so the
    // stored resume decision must be applied even though the original
    // interrupter is long gone.
    const auto result = manager_->abortLoadingSession(UNIVERSE, sentenceTwoResult.getValue().value()->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_TRUE(result.playlistResumed);
    EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Active);
}

TEST_F(SessionManagerInterruptTest, SecondInterruptTakesOverTokenAndDecision) {
    // Interrupt A declines resume; interrupt B (a chain) wants it. B owns the
    // resolution entirely — B's failure must apply B's decision, not A's.
    startInterrupt(/*shouldResume=*/false);
    auto second =
        manager_->interrupt(UNIVERSE, makeAnimation("anim-j", "creature-j"), /*shouldResume=*/true, nullptr, "chain-b");
    ASSERT_TRUE(second.isSuccess());
    ASSERT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Interrupted);

    const auto result = manager_->abortLoadingSession(UNIVERSE, second.getValue().value()->getSessionId());

    EXPECT_TRUE(result.playlistResumed) << "the replacing interrupt's decision must win";
    EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Active);
}

TEST_F(SessionManagerInterruptTest, BystanderFailureDoesNotResolveTheInterrupt) {
    startInterrupt(/*shouldResume=*/true);

    // A non-conflicting bystander adopts alongside the interrupter...
    auto bystander = makeSession("anim-b", "creature-b");
    manager_->registerSession(UNIVERSE, bystander);

    // ...and its failure must leave the interrupt resolution untouched.
    const auto result = manager_->abortLoadingSession(UNIVERSE, bystander->getSessionId());

    EXPECT_TRUE(result.sessionRemoved);
    EXPECT_FALSE(result.playlistResumed);
    EXPECT_FALSE(result.playlistStopped);
    EXPECT_EQ(manager_->getPlaylistState(UNIVERSE), PlaylistState::Interrupted);
}

TEST(SessionManagerOwnership, StaleSessionIdCannotClearNewerState) {
    SessionManager manager;
    manager.startPlaylist(UNIVERSE, "playlist-1");

    auto owner = makePlaylistSession("anim-a", "creature-a");
    manager.registerSession(UNIVERSE, owner, /*isPlaylist=*/true);

    // The owner finished normally and its slot was cleared; a very late abort
    // for its (now unregistered) session id must be a complete no-op.
    manager.clearSession(UNIVERSE, owner->getSessionId());
    const auto result = manager.abortLoadingSession(UNIVERSE, owner->getSessionId());

    EXPECT_FALSE(result.sessionRemoved);
    EXPECT_FALSE(result.playlistCleared);
    EXPECT_EQ(manager.getPlaylistState(UNIVERSE), PlaylistState::Active);
}

} // namespace creatures
