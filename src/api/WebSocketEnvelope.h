#pragma once

#include <algorithm>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "api/JsonResponse.h"
#include "model/JsonCodec.h"

namespace creatures::api {

inline constexpr std::size_t MAX_WEBSOCKET_COMMAND_BYTES = 128;

inline bool isWebSocketCommandToken(std::string_view command) {
    return !command.empty() && std::all_of(command.begin(), command.end(), [](const unsigned char character) {
        return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
               character == '-';
    });
}

struct InboundWebSocketEnvelope {
    std::string command;
    // References the input document; consume the envelope before the document
    // goes out of scope rather than copying attacker-controlled payload bytes.
    std::reference_wrapper<const nlohmann::json> payload;
};

/// Render the stable WebSocket envelope shared by every outbound message.
/// The caller owns payload validation and serialization; this layer only
/// preserves the transport-independent command/payload shape.
inline nlohmann::json webSocketEnvelopeToJson(std::string_view command, const nlohmann::json &payload) {
    return {{"command", command}, {"payload", payload}};
}

inline std::string serializeWebSocketEnvelope(std::string_view command, const nlohmann::json &payload) {
    return serializeJson(webSocketEnvelopeToJson(command, payload)).bytes;
}

inline Result<InboundWebSocketEnvelope> webSocketEnvelopeFromJson(const nlohmann::json &json,
                                                                  std::string_view path = "websocket_message") {
    auto fields = json_codec::rejectUnknownFields(json, path, {"command", "payload"});
    if (!fields.isSuccess()) {
        return Result<InboundWebSocketEnvelope>{fields.getError().value()};
    }
    auto command = json_codec::requiredString(json, path, "command", MAX_WEBSOCKET_COMMAND_BYTES);
    if (!command.isSuccess()) {
        return Result<InboundWebSocketEnvelope>{command.getError().value()};
    }
    if (!isWebSocketCommandToken(command.getValue().value())) {
        return json_codec::invalid<InboundWebSocketEnvelope>(
            fmt::format("{}.command must contain only lowercase ASCII letters, digits, or hyphens", path));
    }
    const auto payload = json.find("payload");
    if (payload == json.end()) {
        return json_codec::invalid<InboundWebSocketEnvelope>(fmt::format("{}.payload is required", path));
    }
    if (!payload->is_object()) {
        return json_codec::invalid<InboundWebSocketEnvelope>(fmt::format("{}.payload must be an object", path));
    }
    return Result<InboundWebSocketEnvelope>{InboundWebSocketEnvelope{command.getValue().value(), std::cref(*payload)}};
}

} // namespace creatures::api
