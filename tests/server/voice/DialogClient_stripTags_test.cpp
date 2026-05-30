#include <string>

#include <gtest/gtest.h>

#include "server/voice/DialogClient.h"

// stripTags is a pure static method — no setup needed. It feeds the
// forced-alignment transcript and the per-character mouth timing math
// (DialogPipeline::assembleChunk consumes `len(stripped)` chars per turn), so
// the behavior on tag handling + whitespace collapsing has to be exact.

using creatures::voice::DialogClient;

TEST(DialogClientStripTags, PassesThroughTextWithNoTags) {
    EXPECT_EQ(DialogClient::stripTags("Hello world"), "Hello world");
}

TEST(DialogClientStripTags, RemovesSingleBracketTag) {
    EXPECT_EQ(DialogClient::stripTags("Hello [giggles] world"), "Hello world");
}

TEST(DialogClientStripTags, RemovesMultipleTags) {
    EXPECT_EQ(DialogClient::stripTags("[whispering] Beaky. [sighs] Wake up."), "Beaky. Wake up.");
}

TEST(DialogClientStripTags, HandlesNestedBrackets) {
    // [[outer [inner]]] — both levels stripped; depth counter prevents output.
    EXPECT_EQ(DialogClient::stripTags("Hi [[nested]] there"), "Hi there");
}

TEST(DialogClientStripTags, UnclosedBracketSwallowsTail) {
    // An unmatched opening bracket eats everything to end of string. Documented
    // behavior — authors should match their brackets.
    EXPECT_EQ(DialogClient::stripTags("Hello [unterminated"), "Hello");
}

TEST(DialogClientStripTags, UnmatchedClosingBracketIsIgnored) {
    // Stray ']' just drops out (depth stays at 0; close is a no-op).
    EXPECT_EQ(DialogClient::stripTags("Hello] world"), "Hello world");
}

TEST(DialogClientStripTags, CollapsesRunsOfWhitespace) {
    EXPECT_EQ(DialogClient::stripTags("a    b\t\tc\n\nd"), "a b c d");
}

TEST(DialogClientStripTags, TrimsLeadingAndTrailingWhitespace) {
    EXPECT_EQ(DialogClient::stripTags("  hello   "), "hello");
    EXPECT_EQ(DialogClient::stripTags("\nfoo\t"), "foo");
}

TEST(DialogClientStripTags, TagRemovalIntroducesNoExtraSpaces) {
    // The character math in assembleChunk depends on this — removing a tag
    // must NOT leave a phantom space behind, or the chars[char_cursor] offset
    // will drift.
    EXPECT_EQ(DialogClient::stripTags("Hi[giggles]there"), "Hithere");
    EXPECT_EQ(DialogClient::stripTags("Hi [giggles]there"), "Hi there");
    EXPECT_EQ(DialogClient::stripTags("Hi[giggles] there"), "Hi there");
}

TEST(DialogClientStripTags, EmptyStringRoundTrip) { EXPECT_EQ(DialogClient::stripTags(""), ""); }

TEST(DialogClientStripTags, OnlyTagsProducesEmpty) {
    EXPECT_EQ(DialogClient::stripTags("[a][b][c]"), "");
    EXPECT_EQ(DialogClient::stripTags("  [tag]  "), "");
}

TEST(DialogClientStripTags, WhitespaceVariantsAllCollapse) {
    // Real-world transcript newlines / tabs / CR / form feed should all behave
    // the same way: collapse to a single ASCII space.
    EXPECT_EQ(DialogClient::stripTags("a\rb\fc\vd"), "a b c d");
}
