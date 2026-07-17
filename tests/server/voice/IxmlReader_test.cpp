#include <string>

#include <gtest/gtest.h>

#include "server/voice/IxmlReader.h"
#include "server/voice/IxmlWriter.h"

using creatures::voice::buildDialogIxml;
using creatures::voice::DialogWavProvenance;
using creatures::voice::extractIxmlField;
using creatures::voice::parseDialogScriptTurns;
using creatures::voice::parseIxmlLipsync;
using creatures::voice::parseIxmlTrackList;
using creatures::voice::parseIxmlWordAlignment;

TEST(IxmlReaderExtract, ReturnsInnerTextOfATag) {
    const std::string xml = "<USER><SOURCE_SCRIPT_ID>abc-123</SOURCE_SCRIPT_ID></USER>";
    auto v = extractIxmlField(xml, "SOURCE_SCRIPT_ID");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "abc-123");
}

TEST(IxmlReaderExtract, ReturnsNulloptForMissingTag) {
    const std::string xml = "<USER><TITLE>hi</TITLE></USER>";
    EXPECT_FALSE(extractIxmlField(xml, "SOURCE_SCRIPT_ID").has_value());
}

TEST(IxmlReaderExtract, UnescapesXmlEntities) {
    const std::string xml = "<DIALOG_SCRIPT>A&amp;B: &lt;&quot;hi&quot;&gt; &apos;x&apos;</DIALOG_SCRIPT>";
    auto v = extractIxmlField(xml, "DIALOG_SCRIPT");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "A&B: <\"hi\"> 'x'");
}

TEST(IxmlReaderExtract, RoundTripsFieldsWrittenByBuildDialogIxml) {
    DialogWavProvenance p;
    p.sourceScriptId = "sid-7";
    p.title = "Scene & <One>";
    p.script = {{"Beaky", "hello & welcome"}, {"Pip", "\"quoted\""}};
    const auto xml = buildDialogIxml(p);

    EXPECT_EQ(extractIxmlField(xml, "SOURCE_SCRIPT_ID").value_or(""), "sid-7");
    EXPECT_EQ(extractIxmlField(xml, "TITLE").value_or(""), "Scene & <One>");
    // The DIALOG_SCRIPT round-trips to the joined, unescaped text.
    EXPECT_EQ(extractIxmlField(xml, "DIALOG_SCRIPT").value_or(""), "Beaky: hello & welcome\nPip: \"quoted\"");
}

// --- Structured parsers (issue #56) ---

TEST(IxmlReaderStructured, ParsesTrackListWrittenByBuildDialogIxml) {
    DialogWavProvenance p;
    p.tracks = {{1, "Beaky"}, {17, "BGM"}};
    // totalChannels forces a complete contiguous 1..17 list with names by channel.
    const auto xml = buildDialogIxml(p, 17);

    const auto tracks = parseIxmlTrackList(xml);
    ASSERT_EQ(tracks.size(), 17u);
    EXPECT_EQ(tracks[0].channel, 1);
    EXPECT_EQ(tracks[0].name, "Beaky");
    EXPECT_EQ(tracks[16].channel, 17);
    EXPECT_EQ(tracks[16].name, "BGM");
    EXPECT_EQ(tracks[5].name, ""); // a silent lane
}

TEST(IxmlReaderStructured, RoundTripsLipsyncCues) {
    DialogWavProvenance p;
    p.lipsync = {{1, "Beaky", {{0.0, 0.25, "A"}, {0.25, 0.5, "B"}}}, {2, "Pip", {{0.1, 0.4, "X"}}}};
    const auto xml = buildDialogIxml(p, 17);

    const auto lipsync = parseIxmlLipsync(xml);
    ASSERT_EQ(lipsync.size(), 2u);
    EXPECT_EQ(lipsync[0].channel, 1);
    EXPECT_EQ(lipsync[0].name, "Beaky");
    ASSERT_EQ(lipsync[0].cues.size(), 2u);
    EXPECT_DOUBLE_EQ(lipsync[0].cues[0].start, 0.0);
    EXPECT_DOUBLE_EQ(lipsync[0].cues[0].end, 0.25);
    EXPECT_EQ(lipsync[0].cues[0].shape, "A");
    EXPECT_EQ(lipsync[0].cues[1].shape, "B");
    EXPECT_EQ(lipsync[1].name, "Pip");
    ASSERT_EQ(lipsync[1].cues.size(), 1u);
    EXPECT_EQ(lipsync[1].cues[0].shape, "X");
}

TEST(IxmlReaderStructured, NoLipsyncOrTracksYieldsEmpty) {
    DialogWavProvenance p;
    p.title = "Just a title";
    const auto xml = buildDialogIxml(p); // no tracks, no lipsync
    EXPECT_TRUE(parseIxmlTrackList(xml).empty());
    EXPECT_TRUE(parseIxmlLipsync(xml).empty());
    EXPECT_TRUE(parseIxmlWordAlignment(xml).empty());
    // Also robust on an empty/garbage document.
    EXPECT_TRUE(parseIxmlTrackList("").empty());
    EXPECT_TRUE(parseIxmlLipsync("<BWFXML></BWFXML>").empty());
}

TEST(IxmlReaderStructured, ParsesWordAlignmentBlock) {
    const std::string xml = "<BWFXML><USER><WORD_ALIGNMENT>"
                            "<TRACK><CHANNEL_INDEX>1</CHANNEL_INDEX><NAME>Beaky</NAME>"
                            "<WORDS>0.000 0.300 hello;0.300 0.600 world</WORDS></TRACK>"
                            "</WORD_ALIGNMENT></USER></BWFXML>";
    const auto words = parseIxmlWordAlignment(xml);
    ASSERT_EQ(words.size(), 1u);
    EXPECT_EQ(words[0].channel, 1);
    EXPECT_EQ(words[0].name, "Beaky");
    ASSERT_EQ(words[0].words.size(), 2u);
    EXPECT_EQ(words[0].words[0].word, "hello");
    EXPECT_DOUBLE_EQ(words[0].words[0].start, 0.0);
    EXPECT_DOUBLE_EQ(words[0].words[0].end, 0.3);
    EXPECT_EQ(words[0].words[1].word, "world");
}

TEST(IxmlReaderStructured, RoundTripsWordAlignmentThroughTheWriter) {
    // #56 Part 2: buildDialogIxml now emits <WORD_ALIGNMENT>; parse it back.
    DialogWavProvenance p;
    p.wordAlignment = {{1, "Beaky", {{"Hello,", 0.0, 0.3}, {"awake?", 0.3, 0.75}}}, {2, "Pip", {{"Yes.", 0.8, 1.1}}}};
    const auto xml = buildDialogIxml(p, 17);

    const auto words = parseIxmlWordAlignment(xml);
    ASSERT_EQ(words.size(), 2u);
    EXPECT_EQ(words[0].channel, 1);
    EXPECT_EQ(words[0].name, "Beaky");
    ASSERT_EQ(words[0].words.size(), 2u);
    EXPECT_EQ(words[0].words[0].word, "Hello,"); // punctuation survives
    EXPECT_DOUBLE_EQ(words[0].words[0].end, 0.3);
    EXPECT_EQ(words[0].words[1].word, "awake?");
    EXPECT_DOUBLE_EQ(words[0].words[1].end, 0.75);
    EXPECT_EQ(words[1].name, "Pip");
    ASSERT_EQ(words[1].words.size(), 1u);
    EXPECT_EQ(words[1].words[0].word, "Yes.");
}

TEST(IxmlReaderStructured, SplitsDialogScriptIntoTurns) {
    const auto turns = parseDialogScriptTurns("Beaky: hello\nPip: bye now");
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_EQ(turns[0].speaker, "Beaky");
    EXPECT_EQ(turns[0].text, "hello");
    EXPECT_EQ(turns[1].speaker, "Pip");
    EXPECT_EQ(turns[1].text, "bye now");
}

TEST(IxmlReaderStructured, ScriptTurnsSplitOnFirstColonSpaceOnly) {
    // A line body that itself contains ": " must survive intact.
    const auto turns = parseDialogScriptTurns("Beaky: well: actually, hi");
    ASSERT_EQ(turns.size(), 1u);
    EXPECT_EQ(turns[0].speaker, "Beaky");
    EXPECT_EQ(turns[0].text, "well: actually, hi");
}

TEST(IxmlReaderStructured, ScriptTurnsHandleNoSpeakerAndBlankLines) {
    const auto turns = parseDialogScriptTurns("no speaker here\n\nBeaky: hi\n");
    ASSERT_EQ(turns.size(), 2u); // blank line skipped
    EXPECT_EQ(turns[0].speaker, "");
    EXPECT_EQ(turns[0].text, "no speaker here");
    EXPECT_EQ(turns[1].speaker, "Beaky");
    EXPECT_EQ(turns[1].text, "hi");
    EXPECT_TRUE(parseDialogScriptTurns("").empty());
}
