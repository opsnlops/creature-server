#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "server/creature/UniverseResolver.h"
#include "util/cache.h"

namespace creatures {

extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;

namespace {

// Real creature ids from the show, so a failure reads like production.
constexpr const char *BEAKY = "4754fc0e-1706-11ef-931d-bbb95a696e2e";
constexpr const char *MANGO = "e93b9a7a-1704-11ef-84b9-3b37dddeb225";
constexpr const char *KENNY = "2e431d6e-175e-11f0-b4f3-cb1f6d25c080";

class UniverseResolverTest : public ::testing::Test {
  protected:
    void SetUp() override { creatureUniverseMap = std::make_shared<ObjectCache<creatureId_t, universe_t>>(); }
    void TearDown() override { creatureUniverseMap.reset(); }

    static void registerOn(const char *creatureId, universe_t universe) {
        creatureUniverseMap->put(creatureId, universe);
    }
};

TEST_F(UniverseResolverTest, ResolvesTheSharedUniverseWhenEveryoneIsRegistered) {
    registerOn(BEAKY, 2);
    registerOn(MANGO, 2);

    const auto resolved = resolvePlaybackUniverse({BEAKY, MANGO});

    ASSERT_TRUE(resolved.isSuccess()) << resolved.getError()->getMessage();
    const auto playback = resolved.getValue().value();
    EXPECT_EQ(playback.universe, 2U);
    EXPECT_TRUE(playback.unregistered.empty());
}

TEST_F(UniverseResolverTest, APoweredOffCreatureDoesNotRefuseTheScene) {
    // The bug that stopped a Creature World response twice: Kenny was down for
    // maintenance and the whole dialog was refused, so nothing talked at all.
    // His audio is a channel in the room mix, so the scene must still play.
    registerOn(BEAKY, 2);
    registerOn(MANGO, 2);

    const auto resolved = resolvePlaybackUniverse({BEAKY, KENNY, MANGO});

    ASSERT_TRUE(resolved.isSuccess()) << resolved.getError()->getMessage();
    const auto playback = resolved.getValue().value();
    EXPECT_EQ(playback.universe, 2U) << "the universe comes from whoever is present";
    ASSERT_EQ(playback.unregistered.size(), 1U);
    EXPECT_EQ(playback.unregistered.front(), KENNY) << "and the absent creature is reported, not dropped";
}

TEST_F(UniverseResolverTest, AbsentCreaturesAreReportedInRequestOrder) {
    registerOn(MANGO, 2);

    const auto resolved = resolvePlaybackUniverse({BEAKY, KENNY, MANGO});

    ASSERT_TRUE(resolved.isSuccess()) << resolved.getError()->getMessage();
    const auto playback = resolved.getValue().value();
    EXPECT_EQ(playback.universe, 2U);
    ASSERT_EQ(playback.unregistered.size(), 2U);
    EXPECT_EQ(playback.unregistered[0], BEAKY);
    EXPECT_EQ(playback.unregistered[1], KENNY);
}

TEST_F(UniverseResolverTest, FailsOnlyWhenNobodyAtAllIsRegistered) {
    // Nothing cached to work from, so any answer would be invented.
    const auto resolved = resolvePlaybackUniverse({BEAKY, KENNY});

    ASSERT_FALSE(resolved.isSuccess());
    EXPECT_EQ(resolved.getError()->getCode(), ServerError::Conflict);
    // Every missing creature is named: reporting only the first sent the
    // operator chasing one bird when several were down.
    const auto message = resolved.getError()->getMessage();
    EXPECT_NE(message.find(BEAKY), std::string::npos);
    EXPECT_NE(message.find(KENNY), std::string::npos);
}

TEST_F(UniverseResolverTest, StillRefusesCreaturesSpanningTwoUniverses) {
    // Playback targets one universe; two rooms at once is a configuration
    // error, not something to degrade through.
    registerOn(BEAKY, 2);
    registerOn(MANGO, 7);

    const auto resolved = resolvePlaybackUniverse({BEAKY, MANGO});

    ASSERT_FALSE(resolved.isSuccess());
    EXPECT_EQ(resolved.getError()->getCode(), ServerError::InvalidData);
}

TEST_F(UniverseResolverTest, AnAbsentCreatureCannotHideAUniverseDisagreement) {
    // The absent one must not short-circuit the scan before the conflict is
    // seen, whatever order the participants arrive in.
    registerOn(BEAKY, 2);
    registerOn(MANGO, 7);

    EXPECT_FALSE(resolvePlaybackUniverse({KENNY, BEAKY, MANGO}).isSuccess());
    EXPECT_FALSE(resolvePlaybackUniverse({BEAKY, KENNY, MANGO}).isSuccess());
    EXPECT_FALSE(resolvePlaybackUniverse({BEAKY, MANGO, KENNY}).isSuccess());
}

TEST_F(UniverseResolverTest, RejectsAnEmptyCreatureList) {
    const auto resolved = resolvePlaybackUniverse({});
    ASSERT_FALSE(resolved.isSuccess());
    EXPECT_EQ(resolved.getError()->getCode(), ServerError::InvalidData);
}

TEST_F(UniverseResolverTest, ReportsAnUnavailableMapAsAnInternalError) {
    creatureUniverseMap.reset();
    const auto resolved = resolvePlaybackUniverse({BEAKY});
    ASSERT_FALSE(resolved.isSuccess());
    EXPECT_EQ(resolved.getError()->getCode(), ServerError::InternalError);
}

} // namespace
} // namespace creatures
