#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include "api/JsonResponse.h"
#include "api/PlaylistRequests.h"
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

TEST(PlaylistJson, RoundTripsStrictNeutralCodec) {
    const Playlist playlist{"11111111-1111-4111-8111-111111111111",
                            "Weighted",
                            {{"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", 2}, {"cccccccc-cccc-4ccc-8ccc-cccccccccccc", 3}},
                            2};
    const auto json = playlistToJson(playlist);
    const auto parsed = playlistFromJson(json);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->id, playlist.id);
    EXPECT_EQ(parsed.getValue()->name, playlist.name);
    EXPECT_EQ(parsed.getValue()->items, playlist.items);
    EXPECT_EQ(parsed.getValue()->number_of_items, playlist.number_of_items);
}

TEST(PlaylistJson, RejectsInvalidAggregateConstraints) {
    nlohmann::json playlist = {{"id", "11111111-1111-4111-8111-111111111111"},
                               {"name", "Weighted"},
                               {"number_of_items", 1},
                               {"items", {{{"animation_id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"}, {"weight", 1}}}}};
    auto mismatchedCount = playlist;
    mismatchedCount["number_of_items"] = 2;
    EXPECT_FALSE(playlistFromJson(mismatchedCount).isSuccess());
    auto unknownField = playlist;
    unknownField["_id"] = "not-client-owned";
    EXPECT_FALSE(playlistFromJson(unknownField).isSuccess());
    auto duplicateAnimation = playlist;
    duplicateAnimation["number_of_items"] = 2;
    duplicateAnimation["items"].push_back(duplicateAnimation["items"][0]);
    EXPECT_FALSE(playlistFromJson(duplicateAnimation).isSuccess());
}

TEST(PlaylistApiContract, StartRequestRequiresAUuidAndKnownFields) {
    const auto valid =
        api::startPlaylistRequestFromJson({{"playlist_id", "11111111-1111-4111-8111-111111111111"}, {"universe", 1}});
    ASSERT_TRUE(valid.isSuccess()) << valid.getError()->getMessage();
    EXPECT_EQ(valid.getValue()->playlistId, "11111111-1111-4111-8111-111111111111");
    EXPECT_EQ(valid.getValue()->universe, 1U);

    EXPECT_FALSE(api::startPlaylistRequestFromJson({{"universe", 1}}).isSuccess());
    EXPECT_FALSE(api::startPlaylistRequestFromJson({{"playlist_id", "not-a-uuid"}, {"universe", 1}}).isSuccess());
    EXPECT_FALSE(api::startPlaylistRequestFromJson(
                     {{"playlist_id", "11111111-1111-4111-8111-111111111111"}, {"universe", 64000}})
                     .isSuccess());
    EXPECT_FALSE(
        api::startPlaylistRequestFromJson({{"playlist_id", "11111111-1111-4111-8111-111111111111"}, {"universe", 0}})
            .isSuccess());
    EXPECT_FALSE(api::startPlaylistRequestFromJson(
                     {{"playlist_id", "11111111-1111-4111-8111-111111111111"}, {"universe", 1}, {"extra", true}})
                     .isSuccess());
}

TEST(PlaylistApiContract, StopRequestRequiresOneBoundedInteger) {
    const auto valid = api::stopPlaylistRequestFromJson({{"universe", 63999}});
    ASSERT_TRUE(valid.isSuccess()) << valid.getError()->getMessage();
    EXPECT_EQ(valid.getValue()->universe, 63999U);

    EXPECT_FALSE(api::stopPlaylistRequestFromJson(nlohmann::json::object()).isSuccess());
    EXPECT_FALSE(api::stopPlaylistRequestFromJson({{"universe", -1}}).isSuccess());
    EXPECT_FALSE(api::stopPlaylistRequestFromJson({{"universe", 0}}).isSuccess());
    EXPECT_FALSE(api::stopPlaylistRequestFromJson({{"universe", true}}).isSuccess());
    EXPECT_FALSE(api::stopPlaylistRequestFromJson({{"universe", 1}, {"extra", true}}).isSuccess());
}

TEST(PlaylistApiContract, StatusResponseOmitsUnavailableSessionId) {
    const auto response = api::statusResponseToJson(api::makeStatusResponse(200, "Started playback"));
    EXPECT_EQ(response, nlohmann::json({{"status", "ok"}, {"code", 200}, {"message", "Started playback"}}));
    EXPECT_FALSE(response.contains("session_id"));
}

} // namespace creatures
