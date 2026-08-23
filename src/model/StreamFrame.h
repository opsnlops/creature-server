
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "server/namespace-stuffs.h"
#include "util/Result.h"

namespace creatures {

/**
 * This is one message that's sent from the client to control a creature in real
 * time. They are sent over the websocket.
 *
 * This is different than `FrameData` because it is not connected to a specific
 * animation, and contains only one frame of data.
 */
struct StreamFrame {
    creatureId_t creature_id;
    universe_t universe;
    std::string data; // The frame data will be base64 encoded strings
};

inline constexpr std::size_t MAX_STREAM_FRAME_DECODED_BYTES = 512;
inline constexpr std::size_t MAX_STREAM_FRAME_ENCODED_BYTES = 684;
inline constexpr uint32_t MAX_E131_UNIVERSE = 63999;

nlohmann::json streamFrameToJson(const StreamFrame &streamFrame);
Result<StreamFrame> streamFrameFromJson(const nlohmann::json &json, std::string_view path = "stream_frame");

} // namespace creatures
