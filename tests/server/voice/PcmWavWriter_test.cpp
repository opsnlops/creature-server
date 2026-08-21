#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/IxmlReader.h"
#include "server/voice/IxmlWriter.h"
#include "server/voice/PcmWavWriter.h"

using creatures::voice::stitchMultichannelWavs;
using creatures::voice::WavProvenance;
using creatures::voice::wrapMonoPcmAsWav;
using creatures::voice::writePcmToMultichannelWav;

namespace {

uint32_t readU32LE(const std::vector<uint8_t> &b, std::size_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}

// 8 bytes of mono S16 PCM (4 samples).
std::vector<uint8_t> samplePcm() { return {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00}; }

// Write `sampleCount` mono samples of the constant `value` as a 17-channel WAV
// part (value on channel `audioChannel`, silence elsewhere), like the streaming
// session does for each sentence.
std::filesystem::path writePart(const std::string &name, int16_t value, std::size_t sampleCount,
                                uint16_t audioChannel) {
    std::vector<uint8_t> pcm(sampleCount * 2);
    for (std::size_t i = 0; i < sampleCount; i++) {
        pcm[i * 2] = static_cast<uint8_t>(value & 0xFF);
        pcm[i * 2 + 1] = static_cast<uint8_t>((value >> 8) & 0xFF);
    }
    const auto path = std::filesystem::temp_directory_path() / name;
    auto result = writePcmToMultichannelWav(pcm, path, audioChannel, 48000);
    EXPECT_TRUE(result.isSuccess());
    return path;
}

std::vector<uint8_t> readAllBytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

} // namespace

TEST(PcmWavWriter, NoProvenanceIsACanonical44ByteWav) {
    const auto pcm = samplePcm();
    const auto wav = wrapMonoPcmAsWav(pcm, 48000);
    EXPECT_EQ(wav.size(), 44u + pcm.size());
    EXPECT_EQ(0, std::memcmp(wav.data(), "RIFF", 4));
    EXPECT_EQ(0, std::memcmp(&wav[8], "WAVE", 4));
    EXPECT_EQ(readU32LE(wav, 4), 36u + pcm.size());
    EXPECT_EQ(readU32LE(wav, 40), pcm.size()) << "data chunk size";
}

TEST(PcmWavWriter, AppendsIxmlChunkWhenProvenancePresent) {
    const auto pcm = samplePcm();

    WavProvenance prov;
    prov.title = "Mono Export";
    prov.script = {{"Beaky", "hello"}, {"Pip", "world"}};
    // Mono callers clear tracks; verify no TRACK_LIST leaks into a 1-channel file.

    const auto wav = wrapMonoPcmAsWav(pcm, 48000, &prov);

    // Audio is untouched.
    EXPECT_EQ(0, std::memcmp(&wav[36], "data", 4));
    EXPECT_EQ(readU32LE(wav, 40), pcm.size());

    // iXML chunk follows the data payload.
    const std::size_t ixmlOffset = 44 + pcm.size();
    ASSERT_GE(wav.size(), ixmlOffset + 8);
    EXPECT_EQ(0, std::memcmp(&wav[ixmlOffset], "iXML", 4));
    const uint32_t payloadSize = readU32LE(wav, ixmlOffset + 4);
    const uint32_t pad = payloadSize % 2;
    EXPECT_EQ(wav.size(), ixmlOffset + 8 + payloadSize + pad);

    // RIFF size covers fmt + data + the whole iXML chunk.
    EXPECT_EQ(readU32LE(wav, 4), 36u + pcm.size() + 8 + payloadSize + pad);

    const std::string text(wav.begin(), wav.end());
    EXPECT_NE(text.find("<BWFXML>"), std::string::npos);
    EXPECT_NE(text.find("Beaky: hello"), std::string::npos);
    EXPECT_EQ(text.find("<TRACK_LIST>"), std::string::npos) << "mono file must not claim a multi-track layout";
}

TEST(PcmWavWriter, EmptyProvenanceAppendsNothing) {
    const auto pcm = samplePcm();
    WavProvenance empty;
    const auto wav = wrapMonoPcmAsWav(pcm, 48000, &empty);
    EXPECT_EQ(wav.size(), 44u + pcm.size()) << "empty provenance adds no chunk";
}

TEST(PcmWavWriter, StitchConcatenatesPartsInOrderWithProvenance) {
    // 4800 samples = 100 ms at 48 kHz; distinct constants tell the parts apart.
    const auto part1 = writePart("creature-server-stitch-p1.wav", 111, 4800, 3);
    const auto part2 = writePart("creature-server-stitch-p2.wav", 222, 9600, 3);
    const auto outPath = std::filesystem::temp_directory_path() / "creature-server-stitch-out.wav";

    WavProvenance provenance;
    provenance.title = "The Whole Exchange";
    provenance.fileUid = "session-uuid";
    provenance.script = {{"Beaky", "Part one."}, {"Beaky", "Part two."}};
    provenance.tracks = {{3, "Beaky"}};

    auto result = stitchMultichannelWavs({part1, part2}, outPath, provenance);
    ASSERT_TRUE(result.isSuccess()) << result.getError()->getMessage();
    const auto stitched = result.getValue().value();
    EXPECT_EQ(100u, stitched.partDurationsMs[0]);
    EXPECT_EQ(200u, stitched.partDurationsMs[1]);
    EXPECT_EQ(300u, stitched.totalDurationMs);

    const auto bytes = readAllBytes(outPath);
    // Header claims 17 channels and the summed data size.
    ASSERT_GE(bytes.size(), 44u);
    EXPECT_EQ(17, bytes[22] | (bytes[23] << 8));
    const uint64_t expectedData = (4800ull + 9600ull) * 17 * 2;
    EXPECT_EQ(expectedData, readU32LE(bytes, 40));

    // First frame carries part 1's value on channel 3, silence on channel 1;
    // the frame right after part 1's samples carries part 2's value.
    auto sampleAt = [&](std::size_t frame, uint16_t channel1Based) -> int16_t {
        const std::size_t offset = 44 + (frame * 17 + (channel1Based - 1)) * 2;
        return static_cast<int16_t>(bytes[offset] | (bytes[offset + 1] << 8));
    };
    EXPECT_EQ(111, sampleAt(0, 3));
    EXPECT_EQ(0, sampleAt(0, 1));
    EXPECT_EQ(111, sampleAt(4799, 3));
    EXPECT_EQ(222, sampleAt(4800, 3));

    // The iXML chunk survives a round trip through the reader.
    auto ixml = creatures::voice::readIxmlChunk(outPath);
    ASSERT_TRUE(ixml.has_value());
    const auto parsed = creatures::voice::parseIxmlProvenance(*ixml);
    EXPECT_EQ("The Whole Exchange", parsed.title);
    EXPECT_EQ("session-uuid", parsed.fileUid);
    ASSERT_EQ(2u, parsed.script.size());
    EXPECT_EQ("Part one.", parsed.script[0].text);

    std::filesystem::remove(part1);
    std::filesystem::remove(part2);
    std::filesystem::remove(outPath);
}

TEST(PcmWavWriter, StitchRejectsEmptyAndMismatchedInputs) {
    const auto outPath = std::filesystem::temp_directory_path() / "creature-server-stitch-bad.wav";
    EXPECT_FALSE(stitchMultichannelWavs({}, outPath, WavProvenance{}).isSuccess());

    // A mono WAV mixed in with a 17-channel part must be refused.
    const auto part = writePart("creature-server-stitch-17ch.wav", 5, 480, 1);
    const auto monoBytes = wrapMonoPcmAsWav(samplePcm(), 48000);
    const auto monoPath = std::filesystem::temp_directory_path() / "creature-server-stitch-mono.wav";
    {
        std::ofstream out(monoPath, std::ios::binary);
        out.write(reinterpret_cast<const char *>(monoBytes.data()), static_cast<std::streamsize>(monoBytes.size()));
    }
    EXPECT_FALSE(stitchMultichannelWavs({part, monoPath}, outPath, WavProvenance{}).isSuccess());

    std::filesystem::remove(part);
    std::filesystem::remove(monoPath);
}
