
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/PcmWavWriter.h"
#include "server/ws/service/SoundRenditionService.h"

namespace {

using creatures::voice::WavProvenance;
using creatures::ws::SoundRenditionFormat;
using creatures::ws::SoundRenditionService;

std::string tagValue(const SoundRenditionService::Comments &comments, const std::string &key) {
    for (const auto &[k, v] : comments) {
        if (k == key) {
            return v;
        }
    }
    return {};
}

TEST(SoundRenditionService, ScriptSpeakersBecomeTheArtist) {
    WavProvenance provenance;
    provenance.title = "The Great Seed Heist";
    provenance.script = {
        {"Beaky", "Where did the seeds go?"}, {"Pip", "I have no idea."}, {"Beaky", "You're covered in millet."}};

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    EXPECT_EQ("The Great Seed Heist", tagValue(comments, "TITLE"));
    // Distinct speakers, in order of first appearance.
    EXPECT_EQ("Beaky, Pip", tagValue(comments, "ARTIST"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ALBUMARTIST"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ALBUM"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "PUBLISHER"));
    EXPECT_EQ("Spoken Word", tagValue(comments, "GENRE"));
    // The transcript doubles as lyrics for players with a lyrics pane.
    EXPECT_EQ("Beaky: Where did the seeds go?\nPip: I have no idea.\nBeaky: You're covered in millet.",
              tagValue(comments, "LYRICS"));
}

TEST(SoundRenditionService, TrackLanesBecomeTheArtistWhenThereIsNoScript) {
    WavProvenance provenance;
    provenance.tracks = {{1, "Beaky"}, {2, "Kenny"}, {17, "BGM"}};

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    // The BGM music lane is not a performer.
    EXPECT_EQ("Beaky, Kenny", tagValue(comments, "ARTIST"));
    // No script and no music recipe: no genre to claim.
    EXPECT_EQ("", tagValue(comments, "GENRE"));
    // But the full channel map still rides along.
    EXPECT_EQ("1: Beaky; 2: Kenny; 17: BGM", tagValue(comments, "TRACK_LIST"));
}

TEST(SoundRenditionService, MirrorsTakeProvenanceIntoTags) {
    WavProvenance provenance;
    provenance.fileUid = "uid-42";
    provenance.take = "take-7";
    provenance.circled = true;
    provenance.generationIds = {"gen-1", "gen-2"};

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    EXPECT_EQ("uid-42", tagValue(comments, "FILE_UID"));
    EXPECT_EQ("take-7", tagValue(comments, "TAKE"));
    EXPECT_EQ("true", tagValue(comments, "CIRCLED"));
    EXPECT_EQ("gen-1, gen-2", tagValue(comments, "GENERATION_IDS"));
}

TEST(SoundRenditionService, WorkshopIsTheArtistOfLastResort) {
    const WavProvenance provenance;

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ARTIST"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ALBUMARTIST"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ALBUM"));
}

TEST(SoundRenditionService, DialogsWithMusicAreTaggedAsSoundtrackNotSpokenWord) {
    WavProvenance provenance;
    provenance.script = {{"Beaky", "How many UUs does it take?"}, {"Mango", "Let's form a committee."}};
    provenance.music.emplace();
    provenance.music->prompt = "gentle hymn underscore";

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    // Music wins the genre even when there's a script (#148).
    EXPECT_EQ("Soundtrack", tagValue(comments, "GENRE"));
    // Everything else still reflects the dialog.
    EXPECT_EQ("Beaky, Mango", tagValue(comments, "ARTIST"));
}

TEST(SoundRenditionService, MusicOnlyRendersAreTaggedAsSoundtrack) {
    WavProvenance provenance;
    provenance.music.emplace();
    provenance.music->prompt = "spooky carnival waltz";
    provenance.music->modelId = "music_v2";
    provenance.tracks = {{1, "BGM"}};

    const auto comments = SoundRenditionService::provenanceTags(provenance);
    EXPECT_EQ("Soundtrack", tagValue(comments, "GENRE"));
    EXPECT_EQ("April's Creature Workshop", tagValue(comments, "ARTIST"));
    EXPECT_EQ("ElevenLabs Music (music_v2)", tagValue(comments, "COMPOSER"));
}

// Write a small mono 48kHz WAV (optionally carrying iXML provenance) to a temp file.
std::filesystem::path writeTempWav(const std::string &name, const WavProvenance *provenance) {
    const std::vector<uint8_t> pcm(9600, 0); // 100ms of silence, S16 mono at 48kHz
    const auto bytes = creatures::voice::wrapMonoPcmAsWav(pcm, 48000, provenance);
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return path;
}

bool containsBytes(const std::vector<uint8_t> &haystack, const std::string &needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

TEST(SoundRenditionService, BorrowsTheFallbackTitleWhenTheWavHasNoProvenance) {
    const auto path = writeTempWav("creature-server-rendition-untitled.wav", nullptr);

    SoundRenditionService service;
    bool consulted = false;
    auto rendition = service.renderWav(path, SoundRenditionFormat::Mp3, [&consulted]() -> std::string {
        consulted = true;
        return "Borrowed Animation Title";
    });
    std::filesystem::remove(path);

    ASSERT_TRUE(rendition.isSuccess());
    EXPECT_TRUE(consulted);
    EXPECT_TRUE(containsBytes(rendition.getValue().value().bytes, "Borrowed Animation Title"));
}

TEST(SoundRenditionService, PrefersTheWavsOwnTitleOverTheFallback) {
    WavProvenance provenance;
    provenance.title = "The Real Title";
    const auto path = writeTempWav("creature-server-rendition-titled.wav", &provenance);

    SoundRenditionService service;
    bool consulted = false;
    auto rendition = service.renderWav(path, SoundRenditionFormat::Mp3, [&consulted]() -> std::string {
        consulted = true;
        return "The Wrong Title";
    });
    std::filesystem::remove(path);

    ASSERT_TRUE(rendition.isSuccess());
    EXPECT_FALSE(consulted);
    EXPECT_TRUE(containsBytes(rendition.getValue().value().bytes, "The Real Title"));
}

} // namespace
