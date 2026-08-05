#include <gtest/gtest.h>

#include "model/DialogScript.h"

TEST(DialogScript, BackgroundMusicRoundTripsThroughJsonAndDto) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music = creatures::DialogBackgroundMusic{
        "dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav", "01234567-89ab-4def-8123-456789abcdef",
        "uneasy chamber strings, restrained and instrumental", 123456789};

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_EQ(json["background_music"]["sound_file"], script.background_music->sound_file);
    EXPECT_EQ(json["background_music"]["generation_id"], script.background_music->generation_id);
    EXPECT_EQ(json["background_music"]["prompt"], script.background_music->prompt);
    EXPECT_EQ(json["background_music"]["accepted_at"], script.background_music->accepted_at);

    const auto dto = creatures::convertToDto(script);
    const auto roundTrip = creatures::convertFromDto(dto.getPtr());
    ASSERT_TRUE(roundTrip.background_music.has_value());
    EXPECT_EQ(*roundTrip.background_music, *script.background_music);
}

TEST(DialogScript, ClearingBackgroundMusicOmitsReferenceFromJson) {
    creatures::DialogScript script;
    script.id = "7aaf11a0-d75d-4ca2-891d-9f3493dc66e4";
    script.title = "Moonlit argument";
    script.turns = {{"55a2af23-e797-462b-8c91-e0bc23b86fd4", "I knew you'd come back."}};
    script.background_music = creatures::DialogBackgroundMusic{
        "dialog/music/moonlit-argument--bgm--uneasy-strings--0123456789ab.wav", "01234567-89ab-4def-8123-456789abcdef",
        "uneasy chamber strings, restrained and instrumental", 123456789};

    script.background_music.reset();

    const auto json = creatures::dialogScriptToJson(script);
    EXPECT_FALSE(json.contains("background_music"));
}

// ===========================================================================
// stage_id round-trip (issue #123)
//
// stage_id was added to the struct, the JSON parser, dialogScriptToJson and
// the DTO — but not to convertToDto, so every read route returned null. The
// value was persisted correctly the whole time; it was dropped on the way out.
//
// Every field on this model needs to survive BOTH round trips, so these tests
// walk each one rather than only the JSON path that happened to be written.
// ===========================================================================

TEST(DialogScriptStageBindingTest, StageIdSurvivesConvertToDto) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "bound scene";
    script.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto dto = creatures::convertToDto(script);
    ASSERT_TRUE(dto->stage_id);
    EXPECT_EQ(std::string(*dto->stage_id), "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329");
}

TEST(DialogScriptStageBindingTest, UnboundScriptOmitsStageIdRatherThanSendingEmpty) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "unbound scene";

    const auto dto = creatures::convertToDto(script);
    EXPECT_FALSE(dto->stage_id) << "an unbound script should omit stage_id, not send an empty string";
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesConvertFromDto) {
    auto dto = creatures::DialogScriptDto::createShared();
    dto->id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    dto->title = "bound scene";
    dto->stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto script = creatures::convertFromDto(dto.getPtr());
    EXPECT_EQ(script.stage_id, "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329");
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesTheFullDtoRoundTrip) {
    creatures::DialogScript original;
    original.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    original.title = "bound scene";
    original.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto dto = creatures::convertToDto(original);
    const auto back = creatures::convertFromDto(dto.getPtr());
    EXPECT_EQ(back.stage_id, original.stage_id);
}

TEST(DialogScriptStageBindingTest, StageIdSurvivesTheJsonRoundTrip) {
    creatures::DialogScript script;
    script.id = "6bf1f0e4-2c9e-4a2f-9f8a-6c7d1e2b3a45";
    script.title = "bound scene";
    script.stage_id = "f22c3002-3b78-4b4a-b9aa-fdf7f4a62329";

    const auto json = creatures::dialogScriptToJson(script);
    ASSERT_TRUE(json.contains("stage_id"));
    EXPECT_EQ(json["stage_id"].get<std::string>(), script.stage_id);

    creatures::DialogScript unbound;
    unbound.id = script.id;
    unbound.title = "unbound";
    EXPECT_FALSE(creatures::dialogScriptToJson(unbound).contains("stage_id"));
}
