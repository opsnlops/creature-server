#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include "util/environment.h"

namespace creatures {
namespace {

constexpr const char *VAR = "CREATURE_TEST_ENVIRONMENT_VALUE";

class EnvironmentTest : public ::testing::Test {
  protected:
    void TearDown() override { ::unsetenv(VAR); }
    static void set(const char *value) { ::setenv(VAR, value, 1); }
};

// --- issue #242: all four helpers must agree that empty means unset ---

TEST_F(EnvironmentTest, AnEmptyValueIsUnsetForEveryHelper) {
    // A bare `FOO=` line is what a systemd EnvironmentFile produces when
    // someone uncomments a setting without filling it in, which the packaged
    // /etc/default/creature-server makes easy. It must mean "not set", not
    // "malformed" — environmentToInt and environmentToDouble used to throw
    // inside std::stoi/std::stod and log "<var> is not an int?" instead.
    set("");

    EXPECT_FALSE(environmentSet(VAR));
    EXPECT_EQ(environmentToInt(VAR, 17), 17);
    EXPECT_EQ(environmentToString(VAR, "fallback"), "fallback");
    EXPECT_DOUBLE_EQ(environmentToDouble(VAR, 2.5), 2.5);
    EXPECT_EQ(environmentToUnsignedLongLong(VAR, 99ULL), 99ULL);
}

TEST_F(EnvironmentTest, AnAbsentVariableIsUnsetForEveryHelper) {
    ::unsetenv(VAR);

    EXPECT_FALSE(environmentSet(VAR));
    EXPECT_EQ(environmentToInt(VAR, 17), 17);
    EXPECT_EQ(environmentToString(VAR, "fallback"), "fallback");
    EXPECT_DOUBLE_EQ(environmentToDouble(VAR, 2.5), 2.5);
    EXPECT_EQ(environmentToUnsignedLongLong(VAR, 99ULL), 99ULL);
}

TEST_F(EnvironmentTest, APresentValueIsUsedByEveryHelper) {
    set("42");

    EXPECT_TRUE(environmentSet(VAR));
    EXPECT_EQ(environmentToInt(VAR, 17), 42);
    EXPECT_EQ(environmentToString(VAR, "fallback"), "42");
    EXPECT_DOUBLE_EQ(environmentToDouble(VAR, 2.5), 42.0);
    EXPECT_EQ(environmentToUnsignedLongLong(VAR, 99ULL), 42ULL);
}

TEST_F(EnvironmentTest, EnvironmentSetSeesWhitespaceAsSet) {
    // Whitespace is a real (if useless) value, and the parsers will reject it.
    // Only truly empty counts as unset, so the two never disagree about
    // whether the operator supplied something.
    set(" ");
    EXPECT_TRUE(environmentSet(VAR));
    EXPECT_EQ(environmentToInt(VAR, 17), 17) << "unparseable, so the default — but it WAS set";
}

TEST_F(EnvironmentTest, AMalformedValueStillFallsBackRatherThanThrowing) {
    set("not-a-number");

    EXPECT_TRUE(environmentSet(VAR)) << "set, just not usable";
    EXPECT_EQ(environmentToInt(VAR, 17), 17);
    EXPECT_DOUBLE_EQ(environmentToDouble(VAR, 2.5), 2.5);
    EXPECT_EQ(environmentToUnsignedLongLong(VAR, 99ULL), 99ULL);
}

TEST_F(EnvironmentTest, EnvironmentSetIsWhatTravelModeNeeds) {
    // The concrete bug: travel mode asked `getenv(...) != nullptr` to decide
    // whether the operator had overridden the loader worker count, so an empty
    // value suppressed travel's safe default of 1 while the parser
    // simultaneously rejected it and returned the mainstage default.
    set("");
    EXPECT_FALSE(environmentSet(VAR)) << "an empty override is not an override";

    set("8");
    EXPECT_TRUE(environmentSet(VAR));
    EXPECT_EQ(environmentToInt(VAR, 1), 8) << "and a real override is honoured";
}

} // namespace
} // namespace creatures
