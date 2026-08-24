#include <gtest/gtest.h>

#include "api/SoundResponses.h"
#include "model/Sound.h"

namespace creatures {

TEST(SoundJsonTest, SerializesLightweightAndStructuredMetadata) {
    Sound sound;
    sound.fileName = "dialog.wav";
    sound.size = 123;
    sound.transcript = "dialog.txt";
    sound.lipsync = "dialog.json";
    sound.title = "Dialog";
    sound.sourceScriptId = "script-id";
    sound.script = "script";
    sound.generationIds = "g1";
    sound.hasEmbeddedScript = true;
    sound.hasEmbeddedLipsync = true;
    sound.scriptTurns.push_back({"Kenny", "Hello"});
    sound.tracks.push_back({1, "Kenny"});
    sound.lipsyncTracks.push_back({1, "Kenny", {{0.0, 0.1, "A"}}});
    sound.wordTracks.push_back({1, "Kenny", {{"Hello", 0.0, 0.1}}});

    const auto json = soundToJson(sound);
    EXPECT_EQ(json.at("file_name"), "dialog.wav");
    EXPECT_EQ(json.at("script_turns").at(0).at("speaker"), "Kenny");
    EXPECT_EQ(json.at("tracks").at(0).at("channel"), 1);
    EXPECT_EQ(json.at("mouth_cues").at(0).at("cues").at(0).at("shape"), "A");
    EXPECT_EQ(json.at("word_timings").at(0).at("words").at(0).at("word"), "Hello");
}

TEST(SoundJsonTest, OmitsEmptyHeavyArrays) {
    Sound sound;
    sound.fileName = "plain.wav";
    sound.size = 1;
    const auto json = soundToJson(sound);
    EXPECT_FALSE(json.contains("script_turns"));
    EXPECT_FALSE(json.contains("tracks"));
    EXPECT_FALSE(json.contains("mouth_cues"));
    EXPECT_FALSE(json.contains("word_timings"));
}

TEST(SoundJsonTest, SerializesAdHocEnvelopeWithoutTransportTypes) {
    Sound sound;
    sound.fileName = "generated.wav";
    const api::AdHocSoundEntry entry{"animation-id", "2026-08-23T00:00:00Z", "/tmp/generated.wav", sound};
    const auto json = api::adHocSoundEntryToJson(entry);
    EXPECT_EQ(json.at("animation_id"), "animation-id");
    EXPECT_EQ(json.at("created_at"), "2026-08-23T00:00:00Z");
    EXPECT_EQ(json.at("sound_file"), "/tmp/generated.wav");
    EXPECT_EQ(json.at("sound").at("file_name"), "generated.wav");
}

} // namespace creatures
