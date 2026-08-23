/*
 * Round-trip regression tests for issue #117.
 *
 * Contract tests for the neutral Animation codec. One characterization test
 * keeps the legacy oat++ null behavior visible until that adapter is removed.
 */

#include <gtest/gtest.h>

#include <base64.hpp>
#include <nlohmann/json.hpp>
#include <oatpp/parser/json/mapping/ObjectMapper.hpp>

#include "model/Animation.h"
#include "server/ws/dto/AnimationDto.h"
#include "util/JsonParser.h"

namespace creatures {

namespace {

/// Serialize through the temporary oat++ HTTP adapter. This intentionally
/// differs from the clean neutral contract while the transport migration is in
/// progress: oat++ emits absent wrapper values as explicit nulls.
nlohmann::json serializeLikeTheLegacyApi(const Animation &animation) {
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
    return animationFromJson(json, AnimationJsonSource::Api);
}

nlohmann::json makeValidAnimationJson() {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));
    return animationToJson(animation);
}

void expectInvalid(const nlohmann::json &json, std::string_view messageFragment,
                   AnimationJsonSource source = AnimationJsonSource::Api) {
    const auto result = animationFromJson(json, source);
    ASSERT_FALSE(result.isSuccess());
    ASSERT_TRUE(result.getError().has_value());
    EXPECT_NE(result.getError()->getMessage().find(messageFragment), std::string::npos)
        << result.getError()->getMessage();
}

} // namespace

TEST(AnimationRoundTripTest, LegacyApiCreatureTrackSerializesFixtureIdAsNull) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    const auto json = serializeLikeTheLegacyApi(animation);

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

    auto result = acceptLikeTheApi(animationToJson(animation));
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

    auto result = acceptLikeTheApi(animationToJson(animation));
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

    auto result = acceptLikeTheApi(animationToJson(animation));
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

    auto result = acceptLikeTheApi(animationToJson(animation));
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");

    const auto parsed = result.getValue().value();
    EXPECT_EQ(parsed.metadata.source_script_id, animation.metadata.source_script_id);
    ASSERT_EQ(parsed.metadata.source_script_turns.size(), 1u);
    EXPECT_EQ(parsed.metadata.source_script_turns[0].text, "Hello there!");
}

TEST(AnimationRoundTripTest, RejectsExplicitNullOptionalNote) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheLegacyApi(animation);
    json["metadata"]["note"] = nullptr;

    auto result = acceptLikeTheApi(json);
    EXPECT_FALSE(result.isSuccess());
}

TEST(AnimationRoundTripTest, PersistenceAcceptsLegacyNullOptionals) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheLegacyApi(animation);
    json["metadata"]["note"] = nullptr;
    json["metadata"]["source_stage_id"] = nullptr;
    json["metadata"]["source_stage_updated_at"] = nullptr;
    json["metadata"]["source_stage_placements"] = nullptr;
    json["metadata"]["render_seed"] = nullptr;
    json["metadata"]["last_updated"] = 1714176000;
    json["metadata"]["source_render_choices"] =
        nlohmann::json::array({{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                                {"speech_loop_animation_id", "eeeeeeee-1111-4222-8333-444455556666"},
                                {"idle_animation_id", ""},
                                {"idle_start_offset", nullptr}}});

    const auto result = animationFromJson(json, AnimationJsonSource::Persistence);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    EXPECT_TRUE(result.getValue()->tracks[0].fixture_id.empty());
    EXPECT_TRUE(result.getValue()->metadata.source_script_id.empty());
    EXPECT_TRUE(result.getValue()->metadata.source_script_turns.empty());
    EXPECT_TRUE(result.getValue()->metadata.source_stage_id.empty());
    ASSERT_EQ(result.getValue()->metadata.source_render_choices.size(), 1u);
    EXPECT_TRUE(result.getValue()->metadata.source_render_choices[0].idle_animation_id.empty());
    EXPECT_EQ(result.getValue()->metadata.source_render_choices[0].idle_start_offset, 0u);
}

TEST(AnimationRoundTripTest, ApiRejectsLegacyNullOptionals) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    const auto json = serializeLikeTheLegacyApi(animation);
    const auto result = animationFromJson(json, AnimationJsonSource::Api);
    EXPECT_FALSE(result.isSuccess());
}

TEST(AnimationRoundTripTest, ParsesFrameDataWithoutReferencingATemporaryTrack) {
    const auto json = makeValidAnimationJson();

    const auto result = animationFromJson(json);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    ASSERT_EQ(result.getValue()->tracks.size(), 1u);
    EXPECT_EQ(result.getValue()->tracks[0].frames, json["tracks"][0]["frames"].get<std::vector<std::string>>());
}

TEST(AnimationRoundTripTest, ApiRejectsSoundFilesOutsideTheSoundLibrary) {
    for (const auto *unsafePath : {"/private/secret.wav", "../../secret.wav", "dialog/../secret.wav"}) {
        auto json = makeValidAnimationJson();
        json["metadata"]["sound_file"] = unsafePath;
        expectInvalid(json, "sound_file must be a normalized relative path");
    }
}

TEST(AnimationRoundTripTest, PersistenceAllowsTrustedAbsoluteAdHocSoundFiles) {
    auto json = makeValidAnimationJson();
    json["metadata"]["sound_file"] = "/tmp/creature-adhoc/session/dialog.wav";

    const auto result = animationFromJson(json, AnimationJsonSource::Persistence);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    EXPECT_EQ(result.getValue()->metadata.sound_file, "/tmp/creature-adhoc/session/dialog.wav");
}

TEST(AnimationRoundTripTest, RejectsTrackWithBothIdsNull) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheLegacyApi(animation);
    json["tracks"][0]["creature_id"] = nullptr;

    // Null-tolerance must not turn into "anything goes" — the XOR rule still holds.
    auto result = acceptLikeTheApi(json);
    EXPECT_FALSE(result.isSuccess());
}

TEST(AnimationRoundTripTest, RejectsNonStringTrackId) {
    auto animation = makeAnimation();
    animation.tracks.push_back(makeCreatureTrack(animation.id));

    auto json = serializeLikeTheLegacyApi(animation);
    json["tracks"][0]["creature_id"] = 42;

    auto result = acceptLikeTheApi(json);
    EXPECT_FALSE(result.isSuccess());
}

TEST(AnimationRoundTripTest, RejectsUnknownFieldsAtEveryTypedLayer) {
    auto topLevel = makeValidAnimationJson();
    topLevel["surprise"] = true;
    expectInvalid(topLevel, "unknown field 'surprise'");

    auto metadata = makeValidAnimationJson();
    metadata["metadata"]["surprise"] = true;
    expectInvalid(metadata, "unknown field 'surprise'");

    auto track = makeValidAnimationJson();
    track["tracks"][0]["surprise"] = true;
    expectInvalid(track, "unknown field 'surprise'");

    auto renderChoice = makeValidAnimationJson();
    renderChoice["metadata"]["source_render_choices"] =
        nlohmann::json::array({{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                                {"speech_loop_animation_id", "ffffffff-1111-4222-8333-444455556666"},
                                {"surprise", true}}});
    expectInvalid(renderChoice, "unknown field 'surprise'");
}

TEST(AnimationRoundTripTest, RejectsMissingRequiredAndWrongTypes) {
    auto missingTitle = makeValidAnimationJson();
    missingTitle["metadata"].erase("title");
    expectInvalid(missingTitle, "metadata.title is required");

    auto wrongFrames = makeValidAnimationJson();
    wrongFrames["tracks"][0]["frames"] = "not-an-array";
    expectInvalid(wrongFrames, "frames must be an array");

    auto wrongBoolean = makeValidAnimationJson();
    wrongBoolean["metadata"]["multitrack_audio"] = 1;
    expectInvalid(wrongBoolean, "multitrack_audio must be a boolean");
}

TEST(AnimationRoundTripTest, RejectsInvalidIdsAndBrokenParentRelationships) {
    auto invalidId = makeValidAnimationJson();
    invalidId["id"] = "not-a-uuid";
    expectInvalid(invalidId, "animation.id must be a UUID");

    auto metadataMismatch = makeValidAnimationJson();
    metadataMismatch["metadata"]["animation_id"] = "99999999-1111-4222-8333-444455556666";
    expectInvalid(metadataMismatch, "metadata.animation_id must equal animation.id");

    auto trackMismatch = makeValidAnimationJson();
    trackMismatch["tracks"][0]["animation_id"] = "99999999-1111-4222-8333-444455556666";
    expectInvalid(trackMismatch, "tracks[0].animation_id must equal animation.id");
}

TEST(AnimationRoundTripTest, RejectsNegativeAndOverflowingIntegers) {
    auto negativeSeed = makeValidAnimationJson();
    negativeSeed["metadata"]["render_seed"] = -1;
    expectInvalid(negativeSeed, "render_seed must not be negative");

    auto overflowingOffset = makeValidAnimationJson();
    overflowingOffset["metadata"]["source_render_choices"] =
        nlohmann::json::array({{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                                {"speech_loop_animation_id", "ffffffff-1111-4222-8333-444455556666"},
                                {"idle_start_offset", uint64_t{1} << 40}}});
    expectInvalid(overflowingOffset, "idle_start_offset exceeds maximum");
}

TEST(AnimationRoundTripTest, EnforcesCollectionAndPayloadBounds) {
    auto tooManyTracks = makeValidAnimationJson();
    tooManyTracks["tracks"] = nlohmann::json::array();
    for (std::size_t index = 0; index <= MAX_ANIMATION_TRACKS; ++index) {
        auto track = makeCreatureTrack(tooManyTracks["id"].get<std::string>());
        track.id = fmt::format("{:08x}-1111-4222-8333-444455556666", index);
        tooManyTracks["tracks"].push_back(trackToJson(track));
    }
    expectInvalid(tooManyTracks, "animation.tracks has 65 entries");

    auto oversizedFrame = makeValidAnimationJson();
    oversizedFrame["tracks"][0]["frames"][0] = std::string(MAX_ANIMATION_FRAME_ENCODED_BYTES + 1, 'x');
    expectInvalid(oversizedFrame, fmt::format("frames[0] is {} bytes", MAX_ANIMATION_FRAME_ENCODED_BYTES + 1));

    auto tooManyChoices = makeValidAnimationJson();
    auto &choices = tooManyChoices["metadata"]["source_render_choices"] = nlohmann::json::array();
    for (std::size_t index = 0; index <= MAX_ANIMATION_RENDER_CHOICES; ++index) {
        choices.push_back({{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                           {"speech_loop_animation_id", "ffffffff-1111-4222-8333-444455556666"}});
    }
    expectInvalid(tooManyChoices, "source_render_choices has 17 entries");

    auto tooManyTotalFrames = makeValidAnimationJson();
    tooManyTotalFrames["metadata"]["number_of_frames"] = MAX_ANIMATION_TOTAL_FRAME_ENTRIES / 3 + 1;
    tooManyTotalFrames["tracks"].push_back(trackToJson(makeFixtureTrack(tooManyTotalFrames["id"].get<std::string>())));
    auto thirdTrack = makeFixtureTrack(tooManyTotalFrames["id"].get<std::string>());
    thirdTrack.id = "cdcdcdcd-1111-4222-8333-444455556666";
    thirdTrack.fixture_id = "dededede-1111-4222-8333-444455556666";
    tooManyTotalFrames["tracks"].push_back(trackToJson(thirdTrack));
    expectInvalid(tooManyTotalFrames, "more than 500000 total frame entries");
}

TEST(AnimationRoundTripTest, RejectsDuplicateProvenanceCreatures) {
    auto duplicateChoices = makeValidAnimationJson();
    const auto choice = nlohmann::json{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                                       {"speech_loop_animation_id", "ffffffff-1111-4222-8333-444455556666"}};
    duplicateChoices["metadata"]["source_render_choices"] = nlohmann::json::array({choice, choice});
    expectInvalid(duplicateChoices, "duplicates an earlier render choice");

    auto duplicatePlacements = makeValidAnimationJson();
    duplicatePlacements["metadata"]["source_stage_id"] = "12121212-1111-4222-8333-444455556666";
    duplicatePlacements["metadata"]["source_stage_updated_at"] = 42;
    const auto placement = nlohmann::json{
        {"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"}, {"x", 0.0}, {"y", 0.0}, {"z", 0.0}, {"yaw", 0.0}};
    duplicatePlacements["metadata"]["source_stage_placements"] = nlohmann::json::array({placement, placement});
    expectInvalid(duplicatePlacements, "duplicates an earlier stage placement");
}

TEST(AnimationRoundTripTest, AllowsTracksShorterThanAnimationDuration) {
    auto json = makeValidAnimationJson();
    json["metadata"]["number_of_frames"] = 3;
    auto fixtureTrack = makeFixtureTrack(json["id"].get<std::string>());
    fixtureTrack.frames.push_back("ZnJhbWUz");
    json["tracks"].push_back(trackToJson(fixtureTrack));

    auto result = animationFromJson(json);

    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto animation = result.getValue().value();
    EXPECT_EQ(animation.metadata.number_of_frames, 3);
    ASSERT_EQ(animation.tracks.size(), 2);
    EXPECT_EQ(animation.tracks[0].frames.size(), 2);
    EXPECT_EQ(animation.tracks[1].frames.size(), 3);
}

TEST(AnimationRoundTripTest, RejectsTracksLongerThanAnimationDuration) {
    auto json = makeValidAnimationJson();
    json["metadata"]["number_of_frames"] = 1;
    expectInvalid(json, "frames has 2 entries, more than metadata.number_of_frames (1)");
}

TEST(AnimationRoundTripTest, RejectsEmptyTracksAndDeclaredDurationWithoutAMatchingTrack) {
    auto emptyTrack = makeValidAnimationJson();
    emptyTrack["tracks"][0]["frames"] = nlohmann::json::array();
    expectInvalid(emptyTrack, "frames must not be empty");

    auto noTrackAtDeclaredDuration = makeValidAnimationJson();
    noTrackAtDeclaredDuration["metadata"]["number_of_frames"] = 3;
    expectInvalid(noTrackAtDeclaredDuration, "must equal the longest track's frame count");
}

TEST(AnimationRoundTripTest, RejectsUnsafePlaybackDuration) {
    auto slowFrames = makeValidAnimationJson();
    slowFrames["metadata"]["milliseconds_per_frame"] = MAX_ANIMATION_MILLISECONDS_PER_FRAME + 1;
    expectInvalid(slowFrames, "milliseconds_per_frame exceeds maximum");

    auto tooLong = makeValidAnimationJson();
    tooLong["metadata"]["number_of_frames"] = MAX_ANIMATION_FRAMES_PER_TRACK;
    tooLong["metadata"]["milliseconds_per_frame"] = MAX_ANIMATION_MILLISECONDS_PER_FRAME;
    tooLong["tracks"] = nlohmann::json::array();
    expectInvalid(tooLong, "animation duration is");
}

TEST(AnimationRoundTripTest, RejectsDuplicateTrackIdsAndTargets) {
    auto duplicateId = makeValidAnimationJson();
    auto secondTrack = makeFixtureTrack(duplicateId["id"].get<std::string>());
    secondTrack.id = duplicateId["tracks"][0]["id"].get<std::string>();
    duplicateId["tracks"].push_back(trackToJson(secondTrack));
    expectInvalid(duplicateId, "id duplicates an earlier track");

    auto duplicateTarget = makeValidAnimationJson();
    auto repeated = makeCreatureTrack(duplicateTarget["id"].get<std::string>());
    repeated.id = "abababab-1111-4222-8333-444455556666";
    duplicateTarget["tracks"].push_back(trackToJson(repeated));
    expectInvalid(duplicateTarget, "duplicates an earlier target");
}

TEST(AnimationRoundTripTest, RejectsInvalidOrOversizedDecodedFrames) {
    auto malformed = makeValidAnimationJson();
    malformed["tracks"][0]["frames"][0] = "not base64!";
    expectInvalid(malformed, "is not valid base64");

    auto empty = makeValidAnimationJson();
    empty["tracks"][0]["frames"][0] = "";
    expectInvalid(empty, "must not decode to empty data");

    auto oversized = makeValidAnimationJson();
    oversized["tracks"][0]["frames"][0] = base64::to_base64(std::string(MAX_ANIMATION_FRAME_DECODED_BYTES + 1, 'x'));
    expectInvalid(oversized, "decodes to 513 bytes");
}

TEST(AnimationRoundTripTest, PersistenceAllowsOnlyDatabaseEnvelopeFields) {
    auto json = makeValidAnimationJson();
    json["_id"] = "database-internal-id";
    json["created_at"] = 1234;

    expectInvalid(json, "unknown field '_id'", AnimationJsonSource::Api);
    const auto result = animationFromJson(json, AnimationJsonSource::Persistence);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
}

TEST(AnimationRoundTripTest, PreservesValidatedStagePlacementExtras) {
    auto json = makeValidAnimationJson();
    json["metadata"]["source_stage_id"] = "12121212-1111-4222-8333-444455556666";
    json["metadata"]["source_stage_updated_at"] = 42;
    json["metadata"]["source_stage_placements"] =
        nlohmann::json::array({{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"},
                                {"x", 1.0},
                                {"y", 2.0},
                                {"z", -3.0},
                                {"yaw", 90.0},
                                {"console_color", "orange"}}});

    const auto result = animationFromJson(json);
    ASSERT_TRUE(result.isSuccess()) << (result.getError() ? result.getError()->getMessage() : "parse failed");
    const auto serialized = animationToJson(result.getValue().value());
    EXPECT_EQ(serialized["metadata"]["source_stage_placements"], json["metadata"]["source_stage_placements"]);
}

TEST(AnimationRoundTripTest, RejectsInvalidStagePlacementGeometry) {
    auto json = makeValidAnimationJson();
    json["metadata"]["source_stage_id"] = "12121212-1111-4222-8333-444455556666";
    json["metadata"]["source_stage_updated_at"] = 42;
    json["metadata"]["source_stage_placements"] = nlohmann::json::array(
        {{{"creature_id", "bbbbbbbb-1111-4222-8333-444455556666"}, {"x", 6.0}, {"y", 0.0}, {"z", 0.0}, {"yaw", 0.0}}});
    expectInvalid(json, "placements[0].x must be finite and between");
}

TEST(AnimationRoundTripTest, ApiJsonParserClassifiesSyntaxAndDepthAsInvalidData) {
    const auto malformed = JsonParser::parseApiJsonString("{", "test animation");
    ASSERT_FALSE(malformed.isSuccess());
    EXPECT_EQ(malformed.getError()->getCode(), ServerError::InvalidData);

    std::string nested(34, '[');
    nested += "0";
    nested.append(34, ']');
    const auto tooDeep = JsonParser::parseApiJsonString(nested, "test animation");
    ASSERT_FALSE(tooDeep.isSuccess());
    EXPECT_EQ(tooDeep.getError()->getCode(), ServerError::InvalidData);
    EXPECT_NE(tooDeep.getError()->getMessage().find("maximum depth"), std::string::npos);
}

} // namespace creatures
