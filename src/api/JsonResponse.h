#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace creatures::api {

inline constexpr const char *STATUS_OK = "ok";
inline constexpr const char *STATUS_ERROR = "error";
inline constexpr const char *STATUS_NOT_FOUND = "not_found";

struct StatusResponse {
    std::string status;
    int code;
    std::string message;
    std::optional<std::string> sessionId;
};

inline const char *defaultStatusForCode(int code) {
    if (code == 404)
        return STATUS_NOT_FOUND;
    if (code >= 200 && code < 300)
        return STATUS_OK;
    return STATUS_ERROR;
}

inline StatusResponse makeStatusResponse(int code, std::string message, const char *statusOverride = nullptr,
                                         std::optional<std::string> sessionId = std::nullopt) {
    return {statusOverride ? statusOverride : defaultStatusForCode(code), code, std::move(message),
            std::move(sessionId)};
}

inline nlohmann::json statusResponseToJson(const StatusResponse &response) {
    nlohmann::json json = {
        {"status", response.status},
        {"code", response.code},
        {"message", response.message},
    };
    if (response.sessionId)
        json["session_id"] = *response.sessionId;
    return json;
}

struct JsonSerialization {
    std::string bytes;
    bool invalidUtf8Replaced;
};

/// Serialize an API response without letting malformed legacy text turn an
/// otherwise-completed request into a second, misleading 500 response. Other
/// serialization errors remain failures rather than being hidden.
inline JsonSerialization serializeJson(const nlohmann::json &json) {
    try {
        return {json.dump(), false};
    } catch (const nlohmann::json::type_error &error) {
        if (error.id != 316)
            throw;
        return {json.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), true};
    }
}

inline std::string jsonToString(const nlohmann::json &json) { return serializeJson(json).bytes; }

template <typename Range, typename Serializer>
nlohmann::json listResponseToJson(const Range &items, Serializer &&serializeItem) {
    auto serializedItems = nlohmann::json::array();
    serializedItems.get_ref<nlohmann::json::array_t &>().reserve(items.size());
    for (const auto &item : items)
        serializedItems.push_back(std::invoke(std::forward<Serializer>(serializeItem), item));
    return {{"count", items.size()}, {"items", std::move(serializedItems)}};
}

} // namespace creatures::api
