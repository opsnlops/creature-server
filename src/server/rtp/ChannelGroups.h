#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "creature/rtp/Protocol.h"

namespace creatures::rtp {

/**
 * The multicast group for a 0-based channel index
 *
 * The server works in 0-based indices — its per-channel arrays, its send loops
 * — while channels on the wire are 1..17, with BGM at 17. That +1 is the whole
 * reason this function exists rather than the conversion being written out at
 * each call site: get it wrong and one creature's audio goes out on the next
 * creature's group, which is audible on real hardware and invisible to any test
 * that doesn't know what address a packet was sent to.
 *
 * @param channelIndex 0-based, as the server's arrays are indexed
 * @throws std::out_of_range if the index is not below CHANNEL_COUNT
 */
[[nodiscard]] inline std::string groupForChannelIndex(std::size_t channelIndex) {
    return channelGroup(static_cast<uint8_t>(channelIndex + 1));
}

} // namespace creatures::rtp
