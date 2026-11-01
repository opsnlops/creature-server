#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

#include <fmt/format.h>

namespace creatures {

/**
 * Narrow an OS interface index to the width the configuration stores, refusing
 * rather than truncating (issue #238).
 *
 * `if_nametoindex()` returns an `unsigned int`. This used to be assigned
 * straight into a `uint8_t`, which silently folded any index above 255 into a
 * small one: index 257 became 1, the "did we find it?" guard passed, and sACN
 * and RTP multicast bound to interface 1 — normally loopback. The creatures
 * received nothing and nothing was logged.
 *
 * Interface indexes are assigned monotonically and never reused within a boot,
 * so the counter climbs with every veth, VLAN, bridge and tun device created
 * since startup. A long-uptime host that churns containers can reach this;
 * a freshly booted one with a few NICs cannot.
 *
 * Split out of CommandLine so it can be tested without pulling argparse into
 * the test binary. `if_nametoindex` itself is a syscall and not worth faking —
 * the part worth testing is what we do with the number it returns.
 *
 * @param index the value from if_nametoindex(); 0 means "no such interface"
 * @param interfaceName the name asked for, for the error message
 * @throws std::system_error when the interface was not found, which is what
 *         the caller already catches
 * @throws std::out_of_range when the index will not fit, so a configuration
 *         the server cannot honour says so instead of quietly working wrong
 */
inline uint16_t narrowInterfaceIndex(unsigned int index, const std::string &interfaceName) {
    if (index == 0) {
        throw std::system_error(ENODEV, std::generic_category(),
                                fmt::format("Failed to find interface: {}", interfaceName));
    }

    constexpr auto widest = std::numeric_limits<uint16_t>::max();
    if (index > widest) {
        throw std::out_of_range(
            fmt::format("Interface {} has index {}, which exceeds the {} this server can store. Truncating it would "
                        "silently bind multicast to a different interface, so refusing instead (issue #238)",
                        interfaceName, index, widest));
    }

    return static_cast<uint16_t>(index);
}

} // namespace creatures
