/*
 * Round-trip regression tests for issue #117.
 *
 * `GET /api/v1/animation/{id}` serializes an AnimationDto, and oatpp writes unset
 * `String` fields as explicit JSON `null`s rather than omitting them. `POST
 * /api/v1/animation` then has to read that back. Before #117 it couldn't: every
 * creature-driven track carries `"fixture_id": null`, and `nlohmann::json::value()`
 * throws on a present-but-null key instead of falling back.
 *
 * These tests drive the real serializer rather than a hand-written JSON blob, so
 * they stay honest if oatpp's null handling ever changes.
 */

#include <gtest/gtest.h>

#include <nlohmann/json.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/Animation.h"
#include "server/database.h"

namespace creatures {

namespace {

/// Serialize an Animation exactly the way the GET endpoint does.
nlohmann::json serializeLikeTheApi(const Animation &animation) {
    auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
    const auto serialized = mapper->writeToString(oatpp::Object<AnimationDto>(convertToDto(animation)));
    return nlohmann::json::parse(std::string(serialized->c_str(), serialized->size()));
}

Animation makeAnimation() {
    Animation animation;
    animation.id = "3f2c1b6e-1111-4222-8333-444455556666";
    animation.metadata.animation_id = animation.id;
    animation.metadata.title = "Beaky Sings";
    animation.metadata.milliseconds_per_frame = 20;
    animation.metadata.note = "";
    animation.metadata.sound_file = "beaky-sings.wav";
    animation.metadata.number_of_frames = 2;
    animation.metadata.multitrack_audio = true;
    return animation;
}

Track makeCreatureTrack(const std::string &animationId) {
    Track track;
    track.id = "aaaaaaaa-1111-4222-8333-444455556666";
    track.creature_id = "bbbbbbbb-1111-4222-8333-444455556666";
    track.animation_id = animationId;
    track.frames = {"ZnJhbWUx", "ZnJhbWUy"};
    return track;
}

Track makeFixtureTrack(const std::string &animationId) {
    Track track;
    track.id = "cccccccc-1111-4222-8333-444455556666";
    track.fixture_id = "dddddddd-1111-4222-8333-444455556666";
    track.animation_id = animationId;
    track.frames = {"ZnJhbWUx", "ZnJhbWUy"};
    return track;
}

/// The POST endpoint validates before parsing, so exercise both.
Result<Animation> acceptLikeTheApi(const nlohmann::json &json) {
    auto valid = Database::validateAnimationJson(json);
    if (!valid.isSuccess()) {
        return Result<Animation>{valid.getError().value()};
    }
    return Database::parseAnimationJson(json);
}

} // namespace

TEST(AnimationRoundTripTest, CreatureTrackSerializesFixtureIdAsNull) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    const auto json = serializeLikeTheApi(animation);

    // This is the shape that broke the round trip. If oatpp ever starts omitting
    // unset fields instead, this expectation is the thing to revisit.
    ASSERT_TRUE(json["tracks"][0].contains("fixture_id"));
    EXPECT_TRUE(json["tracks"][0]["fixture_id"].is_null());
    EXPECT_TRUE(json["metadata"]["source_script_id"].is_null());
    EXPECT_TRUE(json["metadata"]["source_script_turns"].is_null());
}

TEST(AnimationRoundTripTest, CreatureDrivenAnimationSurvivesGetThenPost) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto result = acceptLikeTheApi(serializeLikeTheApi(animation));
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");

    const auto parsed = result.getValue().value();
    EXPECT_EQ(parsed.id, animation.id);
    EXPECT_EQ(parsed.metadata.title, animation.metadata.title);
    EXPECT_EQ(parsed.metadata.sound_file, animation.metadata.sound_file);
    EXPECT_EQ(parsed.metadata.number_of_frames, animation.metadata.number_of_frames);
    EXPECT_TRUE(parsed.metadata.source_script_id.empty());
    EXPECT_TRUE(parsed.metadata.source_script_turns.empty());
    ASSERT_EQ(parsed.tracks.size(), 1u);
    EXPECT_EQ(parsed.tracks[0].creature_id, animation.tracks[0].creature_id);
    EXPECT_TRUE(parsed.tracks[0].fixture_id.empty());
    EXPECT_EQ(parsed.tracks[0].frames, animation.tracks[0].frames);
}

TEST(AnimationRoundTripTest, FixtureDrivenAnimationSurvivesGetThenPost) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeFixtureTrack(animation.id));

    auto result = acceptLikeTheApi(serializeLikeTheApi(animation));
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");

    const auto parsed = result.getValue().value();
    ASSERT_EQ(parsed.tracks.size(), 1u);
    EXPECT_EQ(parsed.tracks[0].fixture_id, animation.tracks[0].fixture_id);
    EXPECT_TRUE(parsed.tracks[0].creature_id.empty());
}

TEST(AnimationRoundTripTest, MixedTracksSurviveGetThenPost) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));
    animation.tracks.push_back(makeFixtureTrack(animation.id));

    auto result = acceptLikeTheApi(serializeLikeTheApi(animation));
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");

    const auto parsed = result.getValue().value();
    ASSERT_EQ(parsed.tracks.size(), 2u);
    EXPECT_EQ(parsed.tracks[0].creature_id, animation.tracks[0].creature_id);
    EXPECT_EQ(parsed.tracks[1].fixture_id, animation.tracks[1].fixture_id);
}

TEST(AnimationRoundTripTest, DialogProvenanceSurvivesGetThenPost) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));
    animation.metadata.source_script_id = "eeeeeeee-1111-4222-8333-444455556666";
    DialogScriptTurn turn;
    turn.creature_id = "bbbbbbbb-1111-4222-8333-444455556666";
    turn.text = "Hello there!";
    animation.metadata.source_script_turns.push_back(turn);

    auto result = acceptLikeTheApi(serializeLikeTheApi(animation));
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");

    const auto parsed = result.getValue().value();
    EXPECT_EQ(parsed.metadata.source_script_id, animation.metadata.source_script_id);
    ASSERT_EQ(parsed.metadata.source_script_turns.size(), 1u);
    EXPECT_EQ(parsed.metadata.source_script_turns[0].text, "Hello there!");
}

TEST(AnimationRoundTripTest, NullNoteIsTreatedAsEmpty) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheApi(animation);
    json["metadata"]["note"] = nullptr;

    auto result = acceptLikeTheApi(json);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    EXPECT_TRUE(result.getValue().value().metadata.note.empty());
}

TEST(AnimationRoundTripTest, RejectsTrackWithBothIdsNull) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheApi(animation);
    json["tracks"][0]["creature_id"] = nullptr;

    // Null-tolerance must not turn into "anything goes" — the XOR rule still holds.
    auto result = acceptLikeTheApi(json);
    EXPECT_FALSE(result.isSuccess());
}

TEST(AnimationRoundTripTest, RejectsNonStringTrackId) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheApi(animation);
    json["tracks"][0]["creature_id"] = 42;

    auto result = acceptLikeTheApi(json);
    EXPECT_FALSE(result.isSuccess());
}

} // namespace creatures
