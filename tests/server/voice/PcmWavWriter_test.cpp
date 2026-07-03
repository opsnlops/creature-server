#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/IxmlWriter.h"
#include "server/voice/PcmWavWriter.h"

using creatures::voice::DialogWavProvenance;
using creatures::voice::wrapMonoPcmAsWav;

namespace {

uint32_t readU32LE(const std::vector<uint8_t> &b, std::size_t o) {
    return static_cast<uint32_t>(b[o]) | (static_cast<uint32_t>(b[o + 1]) << 8) |
           (static_cast<uint32_t>(b[o + 2]) << 16) | (static_cast<uint32_t>(b[o + 3]) << 24);
}

// 8 bytes of mono S16 PCM (4 samples).
std::vector<uint8_t> samplePcm() { return {0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04, 0x00}; }

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

    DialogWavProvenance prov;
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
    DialogWavProvenance empty;
    const auto wav = wrapMonoPcmAsWav(pcm, 48000, &empty);
    EXPECT_EQ(wav.size(), 44u + pcm.size()) << "empty provenance adds no chunk";
}
