#include <array>
#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "creature/rtp/Protocol.h"

#include "server/config.h"
#include "server/rtp/ChannelGroups.h"

namespace creatures::rtp {

/**
 * The multicast addresses the server shipped before the groups were shared.
 *
 * Verbatim from the RTP_GROUPS table that used to live in server/config.h, kept
 * here on purpose: channelGroup() is 1-based and the table was 0-indexed, so the
 * migration turned RTP_GROUPS[i] into channelGroup(i + 1). Get that wrong and one
 * creature's audio goes out on the next creature's group — audible on real
 * hardware, and invisible to every test in creature-common, because none of them
 * know which group a packet is addressed to.
 */
constexpr std::array<const char *, 17> SHIPPED_GROUPS = {
    "239.19.63.1",  "239.19.63.2",  "239.19.63.3",  "239.19.63.4",  "239.19.63.5",  "239.19.63.6",
    "239.19.63.7",  "239.19.63.8",  "239.19.63.9",  "239.19.63.10", "239.19.63.11", "239.19.63.12",
    "239.19.63.13", "239.19.63.14", "239.19.63.15", "239.19.63.16",
    "239.19.63.17" // BGM
};

TEST(ChannelGroupMapping, ChannelGroupsMatchTheAddressesWeShipped) {
    ASSERT_EQ(SHIPPED_GROUPS.size(), static_cast<std::size_t>(CHANNEL_COUNT));

    // groupForChannelIndex() is what the RTP sessions and the RTCP sender
    // actually call, so this tests the conversion they use rather than a
    // re-derivation of it that could be right while they are wrong.
    for (std::size_t channelIndex = 0; channelIndex < SHIPPED_GROUPS.size(); ++channelIndex) {
        EXPECT_EQ(groupForChannelIndex(channelIndex), std::string(SHIPPED_GROUPS[channelIndex]))
            << "channel index " << channelIndex << " (channel " << channelIndex + 1 << ")";
    }
}

TEST(ChannelGroupMapping, EveryChannelIndexResolvesAndTheNextOneDoesNot) {
    for (std::size_t channelIndex = 0; channelIndex < static_cast<std::size_t>(CHANNEL_COUNT); ++channelIndex) {
        EXPECT_NO_THROW(static_cast<void>(groupForChannelIndex(channelIndex))) << "index " << channelIndex;
    }
    EXPECT_THROW(static_cast<void>(groupForChannelIndex(CHANNEL_COUNT)), std::out_of_range);
}

TEST(ChannelGroupMapping, BgmIsTheLastChannel) {
    EXPECT_EQ(static_cast<std::size_t>(BGM_CHANNEL), SHIPPED_GROUPS.size());
    EXPECT_EQ(bgmGroup(), std::string(SHIPPED_GROUPS.back()));
}

TEST(ChannelGroupMapping, TheServerStillRunsSeventeenChannels) {
    // RTP_PCM_BYTES and the memory ceilings in config.h are derived from this,
    // so a change here is a change to the server's budgets too.
    EXPECT_EQ(CHANNEL_COUNT, 17);
    EXPECT_EQ(RTP_PCM_BYTES, 480 * 2 * 17);
}

} // namespace creatures::rtp
