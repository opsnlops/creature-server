#include "gtest/gtest.h"

#include "server/runtime/Activity.h"

using namespace creatures::runtime;

TEST(ActivityEnumTest, ToStringMappings) {
    EXPECT_STREQ("running", toString(ActivityState::Running));
    EXPECT_STREQ("idle", toString(ActivityState::Idle));
    EXPECT_STREQ("disabled", toString(ActivityState::Disabled));
    EXPECT_STREQ("stopped", toString(ActivityState::Stopped));

    EXPECT_STREQ("play", toString(ActivityReason::Play));
    EXPECT_STREQ("playlist", toString(ActivityReason::Playlist));
    EXPECT_STREQ("ad_hoc", toString(ActivityReason::AdHoc));
    EXPECT_STREQ("idle", toString(ActivityReason::Idle));
    EXPECT_STREQ("disabled", toString(ActivityReason::Disabled));
    EXPECT_STREQ("cancelled", toString(ActivityReason::Cancelled));
    EXPECT_STREQ("streaming", toString(ActivityReason::Streaming));
}
