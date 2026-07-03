#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>

#include <string>

#include "server/voice/DialogPipeline.h"
#include "server/voice/DialogWav.h"
#include "server/voice/IxmlWriter.h"
#include "util/ObservabilityManager.h"

#include "../TestGlobals.h"

using creatures::voice::DialogAssembled;
using creatures::voice::DialogPerCreature;
using creatures::voice::VoiceChannelMap;
using creatures::voice::writeDialogWav;

namespace {

/// Read little-endian uint32 at offset.
uint32_t readU32LE(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}
uint16_t readU16LE(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}
int16_t readS16LE(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<int16_t>(readU16LE(bytes, offset));
}

std::vector<uint8_t> readFile(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Build a small two-voice scene where voice A has all 1000s and voice B has
/// all 2000s, distinguishable in the interleaved output.
DialogAssembled twoVoiceFixture(std::size_t samples = 4) {
    DialogAssembled a;
    a.sampleRate = 48000;
    a.totalSamples = samples;
    DialogPerCreature voiceA;
    voiceA.voiceId = "voice-A";
    voiceA.pcm.assign(samples, static_cast<int16_t>(1000));
    DialogPerCreature voiceB;
    voiceB.voiceId = "voice-B";
    voiceB.pcm.assign(samples, static_cast<int16_t>(2000));
    a.perCreature = {voiceA, voiceB};
    return a;
}

class DialogWavTest : public ::testing::Test {
  protected:
    void SetUp() override { creatures::observability = std::make_shared<creatures::ObservabilityManager>(); }
    void TearDown() override {
        creatures::observability.reset();
        std::error_code ec;
        std::filesystem::remove(tmpPath_, ec);
    }
    std::filesystem::path tmpPath_ =
        std::filesystem::temp_directory_path() /
        fmt::format("dialog-wav-test-{}.wav", ::testing::UnitTest::GetInstance()->current_test_info()->name());
};

} // namespace

TEST_F(DialogWavTest, WritesValidPcmWavHeader) {
    auto assembled = twoVoiceFixture(4);
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 2}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_TRUE(r.isSuccess()) << (r.getError() ? r.getError().value().getMessage() : "");

    auto bytes = readFile(tmpPath_);
    ASSERT_GE(bytes.size(), 44u);

    // RIFF / WAVE / fmt / data magic strings.
    EXPECT_TRUE(std::memcmp(&bytes[0], "RIFF", 4) == 0);
    EXPECT_TRUE(std::memcmp(&bytes[8], "WAVE", 4) == 0);
    EXPECT_TRUE(std::memcmp(&bytes[12], "fmt ", 4) == 0);
    EXPECT_TRUE(std::memcmp(&bytes[36], "data", 4) == 0);

    // fmt chunk: PCM (1), 17 channels, 48 kHz, 16 bits/sample.
    EXPECT_EQ(readU32LE(bytes, 16), 16u) << "fmt chunk size";
    EXPECT_EQ(readU16LE(bytes, 20), 1) << "PCM audio format";
    EXPECT_EQ(readU16LE(bytes, 22), 17) << "channels";
    EXPECT_EQ(readU32LE(bytes, 24), 48000u) << "sample rate";
    EXPECT_EQ(readU32LE(bytes, 28), 48000u * 17 * 2) << "byte rate";
    EXPECT_EQ(readU16LE(bytes, 32), 17 * 2) << "block align";
    EXPECT_EQ(readU16LE(bytes, 34), 16) << "bits per sample";

    // data chunk len = samples * 17 channels * 2 bytes.
    EXPECT_EQ(readU32LE(bytes, 40), 4u * 17 * 2);
    EXPECT_EQ(bytes.size(), 44u + 4 * 17 * 2);
}

TEST_F(DialogWavTest, PlacesEachVoiceInItsLaneAndZeroesOthers) {
    auto assembled = twoVoiceFixture(2);
    // voice-A on audio_channel 1 → interleave lane 0.
    // voice-B on audio_channel 5 → interleave lane 4.
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 5}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_TRUE(r.isSuccess());

    auto bytes = readFile(tmpPath_);
    // Sample 0: lanes [0]=1000 (A), [4]=2000 (B), everything else 0.
    for (int lane = 0; lane < 17; ++lane) {
        const int16_t v = readS16LE(bytes, 44 + lane * 2);
        if (lane == 0) {
            EXPECT_EQ(v, 1000) << "voice-A in lane 0";
        } else if (lane == 4) {
            EXPECT_EQ(v, 2000) << "voice-B in lane 4";
        } else {
            EXPECT_EQ(v, 0) << "lane " << lane << " should be zero";
        }
    }
    // Sample 1: same shape (voices have all-same PCM).
    const int sampleOffset = 44 + 17 * 2;
    EXPECT_EQ(readS16LE(bytes, sampleOffset + 0 * 2), 1000);
    EXPECT_EQ(readS16LE(bytes, sampleOffset + 4 * 2), 2000);
    EXPECT_EQ(readS16LE(bytes, sampleOffset + 1 * 2), 0);
}

TEST_F(DialogWavTest, RejectsWrongSampleRate) {
    auto assembled = twoVoiceFixture(2);
    assembled.sampleRate = 44100;
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 2}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST_F(DialogWavTest, RejectsZeroTotalSamples) {
    DialogAssembled a;
    a.sampleRate = 48000;
    a.totalSamples = 0;
    DialogPerCreature pc;
    pc.voiceId = "voice-A";
    a.perCreature = {pc};
    VoiceChannelMap m = {{"voice-A", 1}};
    auto r = writeDialogWav(a, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());
}

TEST_F(DialogWavTest, RejectsVoiceNotInChannelMap) {
    auto assembled = twoVoiceFixture(2);
    VoiceChannelMap m = {{"voice-A", 1}}; // voice-B missing
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST_F(DialogWavTest, RejectsDuplicateChannel) {
    auto assembled = twoVoiceFixture(2);
    VoiceChannelMap m = {{"voice-A", 3}, {"voice-B", 3}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());
    EXPECT_EQ(r.getError().value().getCode(), creatures::ServerError::InvalidData);
}

TEST_F(DialogWavTest, RejectsChannelOutOfRange) {
    auto assembled = twoVoiceFixture(2);
    VoiceChannelMap m = {{"voice-A", 0}, {"voice-B", 2}}; // 0 < 1
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());

    m = {{"voice-A", 1}, {"voice-B", 18}}; // 18 > 17
    auto r2 = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_FALSE(r2.isSuccess());
}

TEST_F(DialogWavTest, RejectsMismatchedPcmLength) {
    DialogAssembled a;
    a.sampleRate = 48000;
    a.totalSamples = 4;
    DialogPerCreature pc;
    pc.voiceId = "voice-A";
    pc.pcm.assign(2, 1); // length 2 != totalSamples 4
    a.perCreature = {pc};
    VoiceChannelMap m = {{"voice-A", 1}};
    auto r = writeDialogWav(a, m, tmpPath_);
    ASSERT_FALSE(r.isSuccess());
}

TEST_F(DialogWavTest, WithoutProvenanceWritesNoTrailingChunk) {
    // The default (nullptr provenance) path is byte-for-byte the canonical file:
    // header + data and nothing after it.
    auto assembled = twoVoiceFixture(4);
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 2}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_TRUE(r.isSuccess());

    auto bytes = readFile(tmpPath_);
    const uint32_t dataBytes = 4u * 17 * 2;
    EXPECT_EQ(bytes.size(), 44u + dataBytes);
    EXPECT_EQ(readU32LE(bytes, 4), 36u + dataBytes) << "RIFF size with no extra chunk";
}

TEST_F(DialogWavTest, EmbedsIxmlProvenanceChunkAfterData) {
    auto assembled = twoVoiceFixture(4);
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 2}};

    creatures::voice::DialogWavProvenance prov;
    prov.sourceScriptId = "script-123";
    prov.title = "Web Scale <Parrots>"; // metachars exercise XML escaping
    prov.generationIds = {"gen-1", "gen-2"};
    prov.tracks = {{1, "Beaky"}, {2, "Pip"}, {17, "BGM"}};
    prov.script = {{"Beaky", "Mongo & \"friends\""}, {"Pip", "web scale!"}};

    auto r = writeDialogWav(assembled, m, tmpPath_, nullptr, &prov);
    ASSERT_TRUE(r.isSuccess()) << (r.getError() ? r.getError().value().getMessage() : "");

    auto bytes = readFile(tmpPath_);
    const uint32_t dataBytes = 4u * 17 * 2;

    // Audio is untouched: the data chunk size and its payload are unchanged, so
    // every SDL/readWavInfo consumer still reads exactly the same samples.
    ASSERT_TRUE(std::memcmp(&bytes[36], "data", 4) == 0);
    EXPECT_EQ(readU32LE(bytes, 40), dataBytes);

    // An iXML chunk follows the data payload.
    const std::size_t ixmlIdOffset = 44 + dataBytes;
    ASSERT_GE(bytes.size(), ixmlIdOffset + 8);
    EXPECT_TRUE(std::memcmp(&bytes[ixmlIdOffset], "iXML", 4) == 0);
    const uint32_t payloadSize = readU32LE(bytes, ixmlIdOffset + 4);
    const uint32_t pad = payloadSize % 2;
    EXPECT_EQ(bytes.size(), ixmlIdOffset + 8 + payloadSize + pad);

    // RIFF outer size covers fmt + data + the whole iXML chunk (header + payload + pad).
    EXPECT_EQ(readU32LE(bytes, 4), 36u + dataBytes + 8 + payloadSize + pad);

    // The provenance is human-findable in the payload, XML-escaped.
    const std::string text(bytes.begin(), bytes.end());
    EXPECT_NE(text.find("<BWFXML>"), std::string::npos);
    EXPECT_NE(text.find("script-123"), std::string::npos);
    EXPECT_NE(text.find("Web Scale &lt;Parrots&gt;"), std::string::npos);
    EXPECT_NE(text.find("Mongo &amp; &quot;friends&quot;"), std::string::npos);
    EXPECT_NE(text.find("gen-1,gen-2"), std::string::npos);
    EXPECT_NE(text.find("<NAME>Beaky</NAME>"), std::string::npos);
}

TEST_F(DialogWavTest, MapEntriesForAbsentVoicesAreIgnored) {
    // Extra entries in voiceToChannel are fine — they just don't show up in
    // the output. Useful if the caller passes the universe's full voice→channel
    // map without filtering.
    auto assembled = twoVoiceFixture(2);
    VoiceChannelMap m = {{"voice-A", 1}, {"voice-B", 2}, {"voice-C-absent", 5}};
    auto r = writeDialogWav(assembled, m, tmpPath_);
    ASSERT_TRUE(r.isSuccess());

    auto bytes = readFile(tmpPath_);
    // Lane 4 (channel 5) should be zero — voice-C wasn't in the scene.
    EXPECT_EQ(readS16LE(bytes, 44 + 4 * 2), 0);
}
