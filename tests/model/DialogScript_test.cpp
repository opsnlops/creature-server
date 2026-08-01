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
