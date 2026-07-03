#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "server/voice/IxmlWriter.h"

using creatures::voice::buildDialogIxml;
using creatures::voice::DialogWavProvenance;
using creatures::voice::makeIxmlChunk;

namespace {

std::string toString(const std::vector<uint8_t> &bytes) { return std::string(bytes.begin(), bytes.end()); }

uint32_t readU32LE(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

DialogWavProvenance sampleProvenance() {
    DialogWavProvenance p;
    p.sourceScriptId = "abc-123";
    p.title = "Scene One";
    p.generationIds = {"g1", "g2", "g3"};
    p.tracks = {{1, "Beaky"}, {2, "Pip"}, {17, "BGM"}};
    p.script = {{"Beaky", "Hello there"}, {"Pip", "Hi!"}};
    return p;
}

} // namespace

TEST(IxmlWriter, EmptyProvenanceReportsEmpty) {
    DialogWavProvenance p;
    EXPECT_TRUE(p.empty());
    p.title = "x";
    EXPECT_FALSE(p.empty());
}

TEST(IxmlWriter, DocumentContainsAllFields) {
    const auto xml = buildDialogIxml(sampleProvenance());
    EXPECT_NE(xml.find("<BWFXML>"), std::string::npos);
    EXPECT_NE(xml.find("<IXML_VERSION>1.5</IXML_VERSION>"), std::string::npos);
    EXPECT_NE(xml.find("<SOURCE_SCRIPT_ID>abc-123</SOURCE_SCRIPT_ID>"), std::string::npos);
    EXPECT_NE(xml.find("<GENERATION_IDS>g1,g2,g3</GENERATION_IDS>"), std::string::npos);
    EXPECT_NE(xml.find("Dialog render: Scene One"), std::string::npos);
    EXPECT_NE(xml.find("<TRACK_COUNT>3</TRACK_COUNT>"), std::string::npos);
    EXPECT_NE(xml.find("<NAME>Beaky</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("<NAME>BGM</NAME>"), std::string::npos);
    EXPECT_NE(xml.find("Beaky: Hello there"), std::string::npos);
    EXPECT_NE(xml.find("Pip: Hi!"), std::string::npos);
}

TEST(IxmlWriter, OmitsTrackListWhenNoTracks) {
    // A mono export carries provenance without a track list (a 17-track list
    // would misdescribe a 1-channel file).
    DialogWavProvenance p;
    p.title = "Mono Take";
    p.script = {{"Beaky", "hi"}};
    const auto xml = buildDialogIxml(p);
    EXPECT_EQ(xml.find("<TRACK_LIST>"), std::string::npos);
    EXPECT_EQ(xml.find("<TRACK_COUNT>"), std::string::npos);
    // The rest of the document is still there.
    EXPECT_NE(xml.find("Beaky: hi"), std::string::npos);
    EXPECT_NE(xml.find("Mono Take"), std::string::npos);
}

TEST(IxmlWriter, EscapesXmlMetacharacters) {
    DialogWavProvenance p;
    p.script = {{"A&B", "he said <\"hi\"> & 'bye'"}};
    const auto xml = buildDialogIxml(p);
    EXPECT_NE(xml.find("A&amp;B: he said &lt;&quot;hi&quot;&gt; &amp; &apos;bye&apos;"), std::string::npos);
    // No raw metacharacter from the payload leaks through (angle brackets only
    // belong to the tags we emit, never to the escaped content).
    EXPECT_EQ(xml.find("<\"hi\">"), std::string::npos);
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
    const auto xml = buildDialogIxml(sampleProvenance());
    const auto chunk = makeIxmlChunk(xml);
    const auto payload = toString({chunk.begin() + 8, chunk.begin() + 8 + xml.size()});
    EXPECT_EQ(payload, xml);
    EXPECT_EQ(readU32LE(chunk, 4), static_cast<uint32_t>(xml.size()));
}
