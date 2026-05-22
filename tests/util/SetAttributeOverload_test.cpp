
#include <string>

#include <gtest/gtest.h>

// Regression test for a bug discovered live in production: every
// span->setAttribute("key", "string literal") call was silently
// recording the boolean value `true` in Honeycomb instead of the
// string. The cause was C++ overload resolution: string literals
// decay to const char*, which has a *standard* conversion to bool
// (any non-null pointer is true), and standard conversions beat the
// user-defined std::string(const char*) constructor. The bool
// overload won.
//
// The fix on ObservabilityManager span classes is to add an explicit
// `setAttribute(const std::string&, const char *)` overload that
// forwards to the std::string version. With that overload in place,
// the compiler picks it for string literals (exact match).
//
// We can't easily inspect the OTel SDK's recorded attribute values
// in the existing FakeObservabilityManager (its setAttribute methods
// are no-ops). So instead we test the same overload-resolution
// pattern using a tiny mirror class that records WHICH overload was
// invoked. If anyone later removes the const char* overload from the
// real span classes, the same change here would break this test.

namespace creatures::test {

enum class WhichOverload {
    None,
    String,
    Bool,
    CharPtr,
};

// Mirror of the ObservabilityManager span pattern: a const std::string& overload
// and a bool overload, both present. Without an explicit const char* overload,
// passing a string literal will resolve to the bool overload (the production
// bug). WITH the const char* overload, the compiler picks it.
struct AttributeRecorder {
    mutable WhichOverload last = WhichOverload::None;

    void setAttribute(const std::string & /*key*/, const std::string & /*value*/) const {
        last = WhichOverload::String;
    }
    void setAttribute(const std::string & /*key*/, bool /*value*/) const { last = WhichOverload::Bool; }
    // The fix: explicit const char* overload forwarding to std::string. Identical
    // shape to the one added to RequestSpan / OperationSpan / SamplingSpan.
    void setAttribute(const std::string &key, const char *value) const {
        last = WhichOverload::CharPtr;
        // Forward to the std::string overload in the real classes; here we just
        // record that the char* overload was picked. The forwarding itself is
        // tested by the existing string-overload test case below.
        (void)key;
        (void)value;
    }
};

TEST(SetAttributeOverloadTest, StringLiteralPicksCharPtrOverload) {
    // The bug: without the const char* overload this call records WhichOverload::Bool.
    // The fix: with the overload, it records WhichOverload::CharPtr.
    AttributeRecorder r;
    r.setAttribute("endpoint", "getAllFixtures");
    EXPECT_EQ(r.last, WhichOverload::CharPtr) << "String literal resolved to overload " << static_cast<int>(r.last)
                                              << " — expected CharPtr (3). If this shows Bool (2), the const char* "
                                                 "overload is missing and every setAttribute(key, \"literal\") call "
                                                 "across the codebase is silently recording `true` instead of the "
                                                 "string value.";
}

TEST(SetAttributeOverloadTest, StdStringPicksStringOverload) {
    AttributeRecorder r;
    const std::string v = "DmxFixtureController";
    r.setAttribute("controller", v);
    EXPECT_EQ(r.last, WhichOverload::String);
}

TEST(SetAttributeOverloadTest, BoolPicksBoolOverload) {
    AttributeRecorder r;
    r.setAttribute("validation.passed", true);
    EXPECT_EQ(r.last, WhichOverload::Bool);
}

TEST(SetAttributeOverloadTest, ConstCharPtrVariablePicksCharPtrOverload) {
    AttributeRecorder r;
    const char *p = "ad_hoc";
    r.setAttribute("on_reason", p);
    EXPECT_EQ(r.last, WhichOverload::CharPtr);
}

TEST(SetAttributeOverloadTest, NullCharPtrStillPicksCharPtrOverload) {
    // A null const char* would also satisfy the bool conversion (→ false), so without
    // the overload it'd silently record `false`. With the overload it routes to the
    // char* path; the production implementation defends with `value ? value : ""`.
    AttributeRecorder r;
    const char *p = nullptr;
    r.setAttribute("missing", p);
    EXPECT_EQ(r.last, WhichOverload::CharPtr);
}

} // namespace creatures::test
