#include <string>

#include <gtest/gtest.h>

#include "server/voice/IxmlReader.h"
#include "server/voice/IxmlWriter.h"

using creatures::voice::buildDialogIxml;
using creatures::voice::DialogWavProvenance;
using creatures::voice::extractIxmlField;

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
