#include <gtest/gtest.h>

#include "model/CacheInvalidation.h"
#include "model/Notice.h"
#include "model/PlaylistStatus.h"

namespace creatures {

TEST(PlaylistStatusJson, PreservesTheExistingWireShape) {
    const PlaylistStatus status{3, "playlist-id", true, "animation-id"};
    const auto json = playlistStatusToJson(status);

    EXPECT_EQ(
        json,
        (nlohmann::json{
            {"universe", 3}, {"playlist", "playlist-id"}, {"playing", true}, {"current_animation", "animation-id"}}));
    const auto parsed = playlistStatusFromJson(json);
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->universe, status.universe);
    EXPECT_EQ(parsed.getValue()->playlist, status.playlist);
    EXPECT_EQ(parsed.getValue()->playing, status.playing);
    EXPECT_EQ(parsed.getValue()->current_animation, status.current_animation);
}

TEST(NoticeJson, RejectsMalformedInboundMessages) {
    const Notice notice{"2026-08-22T12:00:00Z", "Hello"};
    EXPECT_EQ(noticeToJson(notice), (nlohmann::json{{"timestamp", notice.timestamp}, {"message", notice.message}}));
    EXPECT_TRUE(noticeFromJson(noticeToJson(notice)).isSuccess());
    EXPECT_FALSE(noticeFromJson({{"timestamp", notice.timestamp}, {"message", 5}}).isSuccess());
    EXPECT_FALSE(
        noticeFromJson({{"timestamp", notice.timestamp}, {"message", notice.message}, {"extra", true}}).isSuccess());
}

TEST(CacheInvalidationJson, PreservesCacheTypeMapping) {
    const CacheInvalidation invalidation{CacheType::Fixture};
    EXPECT_EQ(cacheInvalidationToJson(invalidation), (nlohmann::json{{"cache_type", "fixture"}}));
    const auto parsed = cacheInvalidationFromJson({{"cache_type", "fixture"}});
    ASSERT_TRUE(parsed.isSuccess()) << parsed.getError()->getMessage();
    EXPECT_EQ(parsed.getValue()->cache_type, CacheType::Fixture);
    const auto unknown = cacheInvalidationFromJson({{"cache_type", "not-a-cache"}});
    ASSERT_TRUE(unknown.isSuccess()) << unknown.getError()->getMessage();
    EXPECT_EQ(unknown.getValue()->cache_type, CacheType::Unknown);
}

} // namespace creatures
