#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/AdHocExchange.h"
#include "model/JsonCodec.h"
#include "util/Result.h"
#include "util/UuidValidation.h"

namespace creatures::api {

inline constexpr std::size_t MAX_STREAMING_AD_HOC_CONTROL_BODY_BYTES = 4096;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_TEXT_BODY_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_STREAMING_AD_HOC_TEXT_BYTES = 32 * 1024;
inline constexpr int DEFAULT_AD_HOC_EXCHANGE_LIMIT = 50;
inline constexpr int MAX_AD_HOC_EXCHANGE_LIMIT = 500;

struct StreamingAdHocStartRequest {
    std::string creatureId;
    bool resumePlaylist{true};
};

struct StreamingAdHocTextRequest {
    std::string sessionId;
    std::string text;
};

struct StreamingAdHocFinishRequest {
    std::string sessionId;
};

struct StreamingAdHocStartResponse {
    std::string sessionId;
    std::string status;
    std::string message;
};

struct StreamingAdHocTextResponse {
    std::string sessionId;
    std::string status;
    int chunksReceived;
};

struct StreamingAdHocFinishResponse {
    std::string sessionId;
    std::string status;
    std::string message;
    std::string animationId;
    bool playbackTriggered;
    std::string exchangeStatus;
    int partsRendered;
    int partsTotal;
};

inline Result<std::string> streamingUuid(const nlohmann::json &json, std::string_view path, std::string_view key) {
    auto value = json_codec::requiredString(json, path, key, 36);
    if (!value.isSuccess())
        return value;
    if (!isUuidShape(value.getValue().value()))
        return json_codec::invalid<std::string>(fmt::format("{}.{} must be a UUID", path, key));
    // Historical Mongo IDs are case-sensitive strings. Preserve submitted
    // spelling for lookups; canonicalize only API output and telemetry until a
    // deliberate data migration has deduplicated case variants.
    return value;
}

inline Result<std::string> streamingSessionUuid(const nlohmann::json &json, std::string_view path,
                                                std::string_view key) {
    auto value = streamingUuid(json, path, key);
    if (!value.isSuccess())
        return value;
    // Session ids originate in this server, so case normalization cannot
    // collide with a historical database key. Accept Swift's uppercase UUIDs
    // and keep the active-session lookup and every response lowercase.
    return Result<std::string>{canonicalUuid(value.getValue().value())};
}

inline Result<StreamingAdHocStartRequest> streamingAdHocStartRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "streaming ad-hoc start request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"creature_id", "resume_playlist"});
    if (!fields.isSuccess())
        return Result<StreamingAdHocStartRequest>{fields.getError().value()};
    auto creatureId = streamingUuid(json, path, "creature_id");
    auto resumePlaylist = json_codec::optionalBool(json, path, "resume_playlist");
    if (!creatureId.isSuccess())
        return Result<StreamingAdHocStartRequest>{creatureId.getError().value()};
    if (!resumePlaylist.isSuccess())
        return Result<StreamingAdHocStartRequest>{resumePlaylist.getError().value()};
    return Result<StreamingAdHocStartRequest>{
        {creatureId.getValue().value(), resumePlaylist.getValue()->value_or(true)}};
}

inline Result<StreamingAdHocTextRequest> streamingAdHocTextRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "streaming ad-hoc text request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"session_id", "text"});
    if (!fields.isSuccess())
        return Result<StreamingAdHocTextRequest>{fields.getError().value()};
    auto sessionId = streamingSessionUuid(json, path, "session_id");
    auto text = json_codec::requiredString(json, path, "text", MAX_STREAMING_AD_HOC_TEXT_BYTES);
    if (!sessionId.isSuccess())
        return Result<StreamingAdHocTextRequest>{sessionId.getError().value()};
    if (!text.isSuccess())
        return Result<StreamingAdHocTextRequest>{text.getError().value()};
    return Result<StreamingAdHocTextRequest>{{sessionId.getValue().value(), text.getValue().value()}};
}

inline Result<StreamingAdHocFinishRequest> streamingAdHocFinishRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "streaming ad-hoc finish request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"session_id"});
    if (!fields.isSuccess())
        return Result<StreamingAdHocFinishRequest>{fields.getError().value()};
    auto sessionId = streamingSessionUuid(json, path, "session_id");
    if (!sessionId.isSuccess())
        return Result<StreamingAdHocFinishRequest>{sessionId.getError().value()};
    return Result<StreamingAdHocFinishRequest>{{sessionId.getValue().value()}};
}

inline Result<int> adHocExchangeLimitFromString(std::string_view value) {
    if (value.empty())
        return json_codec::invalid<int>("limit must be an integer");
    int limit = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), limit);
    if (error != std::errc{} || end != value.data() + value.size())
        return json_codec::invalid<int>("limit must be an integer");
    if (limit < 1 || limit > MAX_AD_HOC_EXCHANGE_LIMIT)
        return json_codec::invalid<int>(fmt::format("limit must be between 1 and {}", MAX_AD_HOC_EXCHANGE_LIMIT));
    return Result<int>{limit};
}

inline nlohmann::json streamingAdHocStartResponseToJson(const StreamingAdHocStartResponse &response) {
    return {{"session_id", response.sessionId}, {"status", response.status}, {"message", response.message}};
}

inline nlohmann::json streamingAdHocTextResponseToJson(const StreamingAdHocTextResponse &response) {
    return {
        {"session_id", response.sessionId}, {"status", response.status}, {"chunks_received", response.chunksReceived}};
}

inline nlohmann::json streamingAdHocFinishResponseToJson(const StreamingAdHocFinishResponse &response) {
    return {{"session_id", response.sessionId},
            {"status", response.status},
            {"message", response.message},
            {"animation_id", response.animationId},
            {"playback_triggered", response.playbackTriggered},
            {"exchange_status", response.exchangeStatus},
            {"parts_rendered", response.partsRendered},
            {"parts_total", response.partsTotal}};
}

inline std::string canonicalUuidForWire(const std::string &value) {
    return isUuidShape(value) ? canonicalUuid(value) : value;
}

inline nlohmann::json adHocExchangeResponseToJson(const AdHocExchange &exchange, const std::string &createdAt,
                                                  const std::optional<std::string> &finishedAt) {
    auto parts = nlohmann::json::array();
    parts.get_ref<nlohmann::json::array_t &>().reserve(exchange.parts.size());
    for (const auto &part : exchange.parts) {
        parts.push_back({{"index", part.index},
                         {"animation_id", canonicalUuidForWire(part.animation_id)},
                         {"text", part.text},
                         {"duration_ms", part.duration_ms}});
    }
    nlohmann::json json = {{"session_id", canonicalUuidForWire(exchange.session_id)},
                           {"creature_id", canonicalUuidForWire(exchange.creature_id)},
                           {"creature_name", exchange.creature_name},
                           {"status", exchange.status},
                           {"title", exchange.title},
                           {"transcript", exchange.transcript},
                           {"duration_ms", exchange.duration_ms},
                           {"created_at", createdAt},
                           {"parts", std::move(parts)}};
    if (finishedAt)
        json["finished_at"] = *finishedAt;
    return json;
}

} // namespace creatures::api
