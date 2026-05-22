
#include <gtest/gtest.h>

#include "server/fixture/FixtureBindingDispatcher.h"

namespace creatures {

using runtime::ActivityReason;
using runtime::ActivityState;

TEST(FixtureBindingMatchTest, BothFiltersNullIsWildcard) {
    EXPECT_TRUE(
        FixtureBindingDispatcher::matches(std::nullopt, std::nullopt, ActivityReason::AdHoc, ActivityState::Running));
    EXPECT_TRUE(
        FixtureBindingDispatcher::matches(std::nullopt, std::nullopt, ActivityReason::Idle, ActivityState::Idle));
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::nullopt, std::nullopt, ActivityReason::Streaming,
                                                  ActivityState::Stopped));
}

TEST(FixtureBindingMatchTest, ReasonFilterMatchesOnlyExactReason) {
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"), std::nullopt,
                                                  ActivityReason::AdHoc, ActivityState::Running));
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"), std::nullopt,
                                                   ActivityReason::Play, ActivityState::Running));
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"), std::nullopt,
                                                   ActivityReason::Idle, ActivityState::Idle));
}

TEST(FixtureBindingMatchTest, StateFilterMatchesOnlyExactState) {
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::nullopt, std::optional<std::string>("running"),
                                                  ActivityReason::AdHoc, ActivityState::Running));
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::nullopt, std::optional<std::string>("running"),
                                                   ActivityReason::AdHoc, ActivityState::Idle));
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::nullopt, std::optional<std::string>("running"),
                                                   ActivityReason::Idle, ActivityState::Stopped));
}

TEST(FixtureBindingMatchTest, BothFiltersBothMustMatch) {
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"),
                                                  std::optional<std::string>("running"), ActivityReason::AdHoc,
                                                  ActivityState::Running));

    // Reason matches, state doesn't.
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"),
                                                   std::optional<std::string>("running"), ActivityReason::AdHoc,
                                                   ActivityState::Idle));

    // State matches, reason doesn't.
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"),
                                                   std::optional<std::string>("running"), ActivityReason::Play,
                                                   ActivityState::Running));

    // Neither matches.
    EXPECT_FALSE(FixtureBindingDispatcher::matches(std::optional<std::string>("ad_hoc"),
                                                   std::optional<std::string>("running"), ActivityReason::Idle,
                                                   ActivityState::Stopped));
}

TEST(FixtureBindingMatchTest, ReasonAndStateStringsAreCanonical) {
    // The filter strings are the exact lowercase forms emitted by runtime::toString.
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::optional<std::string>("streaming"),
                                                  std::optional<std::string>("running"), ActivityReason::Streaming,
                                                  ActivityState::Running));
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::optional<std::string>("playlist"),
                                                  std::optional<std::string>("idle"), ActivityReason::Playlist,
                                                  ActivityState::Idle));
    EXPECT_TRUE(FixtureBindingDispatcher::matches(std::optional<std::string>("cancelled"),
                                                  std::optional<std::string>("stopped"), ActivityReason::Cancelled,
                                                  ActivityState::Stopped));
}

} // namespace creatures
