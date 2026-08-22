#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "model/Playlist.h"

namespace creatures {

namespace {

Playlist weightedPlaylist() {
    return Playlist{"11111111-1111-4111-8111-111111111111",
                    "Weighted",
                    {{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 2},
                     {"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", 0},
                     {"cccccccc-cccc-4ccc-8ccc-cccccccccccc", 3}},
                    3};
}

} // namespace

TEST(PlaylistSelection, CalculatesWeightWithoutExpandingEntries) {
    auto playlist = weightedPlaylist();
    playlist.items[0].weight = std::numeric_limits<uint32_t>::max();
    playlist.items[2].weight = std::numeric_limits<uint32_t>::max();

    const auto result = playlistTotalWeight(playlist);
    ASSERT_TRUE(result.isSuccess());
    EXPECT_EQ(result.getValue().value(), static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) * 2);
}

TEST(PlaylistSelection, MapsEveryWeightBoundaryToTheCorrectAnimation) {
    const auto playlist = weightedPlaylist();

    EXPECT_EQ(playlistAnimationAtWeight(playlist, 0).getValue().value(), playlist.items[0].animation_id);
    EXPECT_EQ(playlistAnimationAtWeight(playlist, 1).getValue().value(), playlist.items[0].animation_id);
    EXPECT_EQ(playlistAnimationAtWeight(playlist, 2).getValue().value(), playlist.items[2].animation_id);
    EXPECT_EQ(playlistAnimationAtWeight(playlist, 4).getValue().value(), playlist.items[2].animation_id);
}

TEST(PlaylistSelection, RejectsEmptyAndOutOfRangeSelections) {
    const Playlist emptyPlaylist{"22222222-2222-4222-8222-222222222222", "Empty", {}, 0};
    const auto emptyTotal = playlistTotalWeight(emptyPlaylist);
    ASSERT_TRUE(emptyTotal.isSuccess());
    EXPECT_EQ(emptyTotal.getValue().value(), 0);

    const auto emptySelection = playlistAnimationAtWeight(emptyPlaylist, 0);
    EXPECT_FALSE(emptySelection.isSuccess());

    const auto outOfRange = playlistAnimationAtWeight(weightedPlaylist(), 5);
    EXPECT_FALSE(outOfRange.isSuccess());
}

} // namespace creatures
