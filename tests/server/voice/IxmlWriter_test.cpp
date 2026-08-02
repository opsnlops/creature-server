#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/IxmlWriter.h"

using creatures::voice::buildIxml;
using creatures::voice::DialogLipsyncTrack;
using creatures::voice::makeIxmlChunk;
using creatures::voice::WavProvenance;

namespace {

std::string toString(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

uint32_t readU32LE(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

WavProvenance sampleProvenance() {
    WavProvenance p;
    p.sourceScriptId = "abc-123";
    p.title = "Scene One";
    p.generationIds = {"g1", "g2", "g3"};
    p.tracks = {{1, "Beaky"}, {2, "Pip"}, {17, "BGM"}};
    p.script = {{"Beaky", "Hello there"}, {"Pip", "Hi!"}};
    return p;
}

} // namespace

TEST(IxmlWriter, EmptyProvenanceReportsEmpty) {
    WavProvenance p;
    EXPECT_TRUE(p.empty());
    p.title = "x";
    EXPECT_FALSE(p.empty());
}

TEST(IxmlWriter, DocumentContainsAllFields) {
    const auto xml = buildIxml(sampleProvenance());
    EXPECT_NE(xml.find("<BWFXML>"), std::string::npos);
    EXPECT_NE(xml.find("<IXML_VERSION>2.0</IXML_VERSION>"), std::string::npos);
    EXPECT_NE(xml.find("<SOURCE_SCRIPT_ID>abc-123</SOURCE_SCRIPT_ID>"), std::string::npos);
    EXPECT_NE(xml.find("<GENERATION_IDS>g1,g2,g3</GENERATION_IDS>"), std::string::npos);
    EXPECT_NE(xml.find("Dialog render: Scene One"), std::string::npos);
    EXPECT_NE(xml.find("<TRACK_COUNT>3</TRACK_COUNT>"), std::string::npos);
    EXPECT_NE(xml.find("<NAME>Beaky</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("<NAME>BGM</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("Beaky: Hello there"), std::string::npos);
    EXPECT_NE(xml.find("Pip: Hi!"), std::string::npos);
}

TEST(IxmlWriter, EmitsStandardSlateFieldsAndPrivateMusicRecipe) {
    WavProvenance p;
    p.fileUid = "music-file-1";
    p.take = "take-7";
    p.circled = true;
    p.title = "Library Scene";
    p.tracks = {{1, "BGM"}};
    creatures::voice::MusicWavProvenance music;
    music.provider = "ElevenLabs";
    music.modelId = "music_v2";
    music.prompt = "Mysterious & playful";
    music.sourceDialogDurationMs = 4200;
    music.durationExtensionMs = 1800;
    music.musicLengthMs = 6000;
    music.requestJson = R"({"force_instrumental":true})";
    music.musicGenerationId = "music-file-1";
    p.music = music;

    const auto xml = buildIxml(p);
    EXPECT_NE(xml.find("<SCENE>Library Scene</SCENE>"), std::string::npos);
    EXPECT_NE(xml.find("<TAKE>take-7</TAKE>"), std::string::npos);
    EXPECT_NE(xml.find("<CIRCLED>TRUE</CIRCLED>"), std::string::npos);
    EXPECT_NE(xml.find("<FILE_UID>music-file-1</FILE_UID>"), std::string::npos);
    EXPECT_NE(xml.find("<FILE_SAMPLE_RATE>48000</FILE_SAMPLE_RATE>"), std::string::npos);
    EXPECT_NE(xml.find("<AUDIO_BIT_DEPTH>16</AUDIO_BIT_DEPTH>"), std::string::npos);
    EXPECT_NE(xml.find("<TRACK_COUNT>1</TRACK_COUNT>"), std::string::npos);
    EXPECT_NE(xml.find("<MUSIC_PROVENANCE>"), std::string::npos);
    EXPECT_NE(xml.find("Mysterious &amp; playful"), std::string::npos);
    EXPECT_NE(xml.find("<SOURCE_DIALOG_DURATION_MS>4200</SOURCE_DIALOG_DURATION_MS>"), std::string::npos);
    EXPECT_NE(xml.find("<DURATION_EXTENSION_MS>1800</DURATION_EXTENSION_MS>"), std::string::npos);
    EXPECT_NE(xml.find("<MUSIC_LENGTH_MS>6000</MUSIC_LENGTH_MS>"), std::string::npos);
}

TEST(IxmlWriter, TotalChannelsEmitsCompleteContiguousTrackList) {
    // The used lanes are sparse (1, 2, 17), but a poly WAV needs a TRACK per
    // channel with TRACK_COUNT == channel count for Wave Agent / DAWs to show
    // the names.
    WavProvenance p;
    p.tracks = {{1, "Beaky"}, {2, "Mango"}, {17, "BGM"}};
    const auto xml = buildIxml(p, 17);

    EXPECT_NE(xml.find("<TRACK_COUNT>17</TRACK_COUNT>"), std::string::npos);
    // Named lanes present.
    EXPECT_NE(xml.find("<CHANNEL_INDEX>1</CHANNEL_INDEX><NAME>Beaky</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("<CHANNEL_INDEX>2</CHANNEL_INDEX><NAME>Mango</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("<CHANNEL_INDEX>17</CHANNEL_INDEX><NAME>BGM</NAME>"), std::string::npos);
    // A silent lane is still present, with an empty name.
    EXPECT_NE(xml.find("<CHANNEL_INDEX>5</CHANNEL_INDEX><NAME></NAME>"), std::string::npos);
    // Exactly 17 TRACK entries.
    std::size_t count = 0, pos = 0;
    while ((pos = xml.find("<TRACK>", pos)) != std::string::npos) {
        ++count;
        pos += 7;
    }
    EXPECT_EQ(count, 17u);
}

TEST(IxmlWriter, OmitsTrackListWhenNoTracks) {
    // A mono export carries provenance without a track list (a 17-track list
    // would misdescribe a 1-channel file).
    WavProvenance p;
    p.title = "Mono Take";
    p.script = {{"Beaky", "hi"}};
    const auto xml = buildIxml(p);
    EXPECT_EQ(xml.find("<TRACK_LIST>"), std::string::npos);
    EXPECT_EQ(xml.find("<TRACK_COUNT>"), std::string::npos);
    // The rest of the document is still there.
    EXPECT_NE(xml.find("Beaky: hi"), std::string::npos);
    EXPECT_NE(xml.find("Mono Take"), std::string::npos);
}

TEST(IxmlWriter, EmitsLipsyncBlockWithPackedCues) {
    WavProvenance p;
    DialogLipsyncTrack track;
    track.channel = 1;
    track.name = "Beaky";
    track.cues = {{0.0, 0.12, "B"}, {0.12, 0.25, "A"}, {0.25, 0.4, "X"}};
    p.lipsync = {track};

    const auto xml = buildIxml(p);
    EXPECT_NE(xml.find("<LIPSYNC>"), std::string::npos);
    EXPECT_NE(xml.find("<CHANNEL_INDEX>1</CHANNEL_INDEX><NAME>Beaky</NAME>"), std::string::npos);
    // Cues packed as "start end shape;..." with 3-decimal seconds.
    EXPECT_NE(xml.find("<CUES>0.000 0.120 B;0.120 0.250 A;0.250 0.400 X</CUES>"), std::string::npos);
}

TEST(IxmlWriter, OmitsLipsyncBlockWhenAbsent) {
    WavProvenance p;
    p.script = {{"Beaky", "hi"}};
    EXPECT_EQ(buildIxml(p).find("<LIPSYNC>"), std::string::npos);
}

TEST(IxmlWriter, EscapesXmlMetacharacters) {
    WavProvenance p;
    p.script = {{"A&B", "he said <\"hi\"> & 'bye'"}};
    const auto xml = buildIxml(p);
    EXPECT_NE(xml.find("A&amp;B: he said &lt;&quot;hi&quot;&gt; &amp; &apos;bye&apos;"), std::string::npos);
    // No raw metacharacter from the payload leaks through (angle brackets only
    // belong to the tags we emit, never to the escaped content).
    EXPECT_EQ(xml.find("<\"hi\">"), std::string::npos);
}

TEST(IxmlWriter, ReplacesXml10ForbiddenControlCharacters) {
    WavProvenance p;
    p.title = std::string("scene\0name", 10);
    const auto xml = buildIxml(p);
    EXPECT_EQ(xml.find('\0'), std::string::npos);
    EXPECT_NE(xml.find("<SCENE>scene name</SCENE>"), std::string::npos);
}

TEST(IxmlWriter, ChunkHasCorrectIdAndLittleEndianSize) {
    const std::string doc = "hello"; // 5 bytes, odd → needs a pad byte
    const auto chunk = makeIxmlChunk(doc);

    ASSERT_GE(chunk.size(), 8u);
    EXPECT_EQ(std::string(chunk.begin(), chunk.begin() + 4), "iXML");
    EXPECT_EQ(readU32LE(chunk, 4), 5u) << "size field is the payload length, not padded";

    // 4 (id) + 4 (size) + 5 (payload) + 1 (pad to even) = 14.
    EXPECT_EQ(chunk.size(), 14u);
    EXPECT_EQ(chunk.back(), 0) << "pad byte is zero";
    EXPECT_EQ(std::string(chunk.begin() + 8, chunk.begin() + 13), "hello");
}

TEST(IxmlWriter, ChunkEvenPayloadHasNoPad) {
    const std::string doc = "even"; // 4 bytes
    const auto chunk = makeIxmlChunk(doc);
    EXPECT_EQ(chunk.size(), 12u); // 8 + 4, no pad
    EXPECT_EQ(readU32LE(chunk, 4), 4u);
}

TEST(IxmlWriter, RealDocumentRoundTripsThroughChunk) {
    const auto xml = buildIxml(sampleProvenance());
    const auto chunk = makeIxmlChunk(xml);
    const auto payload = toString({chunk.begin() + 8, chunk.begin() + 8 + xml.size()});
    EXPECT_EQ(payload, xml);
    EXPECT_EQ(readU32LE(chunk, 4), static_cast<uint32_t>(xml.size()));
}
