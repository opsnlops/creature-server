#include <limits>
#include <stdexcept>
#include <system_error>

#include <gtest/gtest.h>

#include "server/config/InterfaceIndex.h"

namespace creatures {
namespace {

TEST(InterfaceIndexTest, PassesThroughAnOrdinaryIndex) {
    EXPECT_EQ(narrowInterfaceIndex(1, "lo"), 1);
    EXPECT_EQ(narrowInterfaceIndex(28, "enp5s0f0np0"), 28);
    EXPECT_EQ(narrowInterfaceIndex(255, "eth0"), 255);
}

TEST(InterfaceIndexTest, AcceptsIndexesAboveTheOldByteCeiling) {
    // The bug: these used to truncate. 256 became 0 and failed with a
    // misleading "interface not found"; 257 became 1 and SILENTLY bound
    // multicast to interface 1, normally loopback (issue #238).
    EXPECT_EQ(narrowInterfaceIndex(256, "veth-heavy-host"), 256);
    EXPECT_EQ(narrowInterfaceIndex(257, "veth-heavy-host"), 257);
    EXPECT_EQ(narrowInterfaceIndex(4096, "vlan-everything"), 4096);
    EXPECT_EQ(narrowInterfaceIndex(std::numeric_limits<uint16_t>::max(), "the-very-last-one"),
              std::numeric_limits<uint16_t>::max());
}

TEST(InterfaceIndexTest, ZeroMeansNoSuchInterfaceAndThrowsSystemError) {
    // if_nametoindex returns 0 for an unknown name. system_error specifically,
    // because that is what the caller in CommandLine already catches.
    try {
        narrowInterfaceIndex(0, "wlan-does-not-exist");
        FAIL() << "a missing interface must throw";
    } catch (const std::system_error &e) {
        EXPECT_NE(std::string(e.what()).find("wlan-does-not-exist"), std::string::npos)
            << "the message must name the interface asked for";
    } catch (...) {
        FAIL() << "must be a std::system_error so the existing catch still handles it";
    }
}

TEST(InterfaceIndexTest, RefusesAnIndexTooLargeToStoreRatherThanTruncating) {
    // The whole point: an index we cannot represent must say so, not fold into
    // a plausible-looking one. Binding multicast to the wrong interface loses
    // the show silently.
    constexpr auto tooBig = static_cast<unsigned int>(std::numeric_limits<uint16_t>::max()) + 1;
    try {
        narrowInterfaceIndex(tooBig, "absurdly-high");
        FAIL() << "an unrepresentable index must throw";
    } catch (const std::out_of_range &e) {
        const std::string message = e.what();
        EXPECT_NE(message.find("absurdly-high"), std::string::npos) << "name the interface";
        EXPECT_NE(message.find("65536"), std::string::npos) << "and the offending index";
    } catch (...) {
        FAIL() << "must be std::out_of_range, which the caller handles separately from not-found";
    }
}

TEST(InterfaceIndexTest, TheTwoFailuresAreDistinguishable) {
    // Not-found and too-large are different operator problems and must not be
    // reported as the same thing — conflating them is what made the old
    // behaviour confusing even when it did fail.
    EXPECT_THROW(narrowInterfaceIndex(0, "nope"), std::system_error);
    EXPECT_THROW(narrowInterfaceIndex(70000, "nope"), std::out_of_range);
}

} // namespace
} // namespace creatures
