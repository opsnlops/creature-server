#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "util/Result.h"

namespace creatures::json_codec {

inline std::string diagnosticKey(std::string_view key) {
    constexpr std::size_t maximumBytes = 64;
    std::string escaped;
    escaped.reserve(std::min(key.size(), maximumBytes) + 16);
    escaped.push_back('\'');
    const auto prefix = key.substr(0, maximumBytes);
    for (const auto character : prefix) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isprint(byte) && character != '\\' && character != '\'') {
            escaped.push_back(character);
        } else {
            escaped += fmt::format("\\x{:02x}", byte);
        }
    }
    escaped.push_back('\'');
    if (key.size() > maximumBytes) {
        escaped += fmt::format(" ({} bytes)", key.size());
    }
    return escaped;
}

template <typename T> Result<T> invalid(std::string message) {
    return Result<T>{ServerError(ServerError::InvalidData, std::move(message))};
}

inline Result<void> requireObject(const nlohmann::json &json, std::string_view path) {
    if (!json.is_object()) {
        return Result<void>{ServerError(ServerError::InvalidData, fmt::format("{} must be an object", path))};
    }
    return Result<void>{};
}

inline Result<void> rejectUnknownFields(const nlohmann::json &json, std::string_view path,
                                        std::initializer_list<std::string_view> allowed) {
    auto objectResult = requireObject(json, path);
    if (!objectResult.isSuccess()) {
        return objectResult;
    }
    for (const auto &[key, unused] : json.items()) {
        static_cast<void>(unused);
        bool known = false;
        for (const auto candidate : allowed) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            return Result<void>{ServerError(ServerError::InvalidData,
                                            fmt::format("{} contains unknown field {}", path, diagnosticKey(key)))};
        }
    }
    return Result<void>{};
}

inline Result<std::string> requiredString(const nlohmann::json &json, std::string_view path, std::string_view key,
                                          std::size_t maxBytes, bool allowEmpty = false) {
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<std::string>(fmt::format("{} is required", fieldPath));
    }
    if (!iterator->is_string()) {
        return invalid<std::string>(fmt::format("{} must be a string", fieldPath));
    }
    auto value = iterator->get<std::string>();
    if (!allowEmpty && value.empty()) {
        return invalid<std::string>(fmt::format("{} must not be empty", fieldPath));
    }
    if (value.size() > maxBytes) {
        return invalid<std::string>(fmt::format("{} is {} bytes; maximum is {}", fieldPath, value.size(), maxBytes));
    }
    return Result<std::string>{value};
}

inline Result<std::optional<std::string>> optionalString(const nlohmann::json &json, std::string_view path,
                                                         std::string_view key, std::size_t maxBytes,
                                                         bool allowEmpty = false, bool allowNull = false) {
    const auto iterator = json.find(key);
    if (iterator == json.end() || (allowNull && iterator->is_null())) {
        return Result<std::optional<std::string>>{std::optional<std::string>{}};
    }
    auto valueResult = requiredString(json, path, key, maxBytes, allowEmpty);
    if (!valueResult.isSuccess()) {
        return Result<std::optional<std::string>>{valueResult.getError().value()};
    }
    return Result<std::optional<std::string>>{std::optional<std::string>{valueResult.getValue().value()}};
}

template <typename T>
Result<T> requiredUnsigned(const nlohmann::json &json, std::string_view path, std::string_view key, T maximum) {
    static_assert(std::is_unsigned_v<T>);
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<T>(fmt::format("{} is required", fieldPath));
    }
    uint64_t value = 0;
    if (iterator->is_number_unsigned()) {
        value = iterator->get<uint64_t>();
    } else if (iterator->is_number_integer()) {
        const auto signedValue = iterator->get<int64_t>();
        if (signedValue < 0) {
            return invalid<T>(fmt::format("{} must not be negative", fieldPath));
        }
        value = static_cast<uint64_t>(signedValue);
    } else {
        return invalid<T>(fmt::format("{} must be an integer", fieldPath));
    }
    if (value > static_cast<uint64_t>(maximum)) {
        return invalid<T>(fmt::format("{} exceeds maximum {}", fieldPath, maximum));
    }
    return Result<T>{static_cast<T>(value)};
}

template <typename T>
Result<std::optional<T>> optionalUnsigned(const nlohmann::json &json, std::string_view path, std::string_view key,
                                          T maximum = std::numeric_limits<T>::max(), bool allowNull = false) {
    const auto iterator = json.find(key);
    if (iterator == json.end() || (allowNull && iterator->is_null())) {
        return Result<std::optional<T>>{std::optional<T>{}};
    }
    auto valueResult = requiredUnsigned<T>(json, path, key, maximum);
    if (!valueResult.isSuccess()) {
        return Result<std::optional<T>>{valueResult.getError().value()};
    }
    return Result<std::optional<T>>{std::optional<T>{valueResult.getValue().value()}};
}

inline Result<int64_t> requiredInt64(const nlohmann::json &json, std::string_view path, std::string_view key,
                                     int64_t minimum = std::numeric_limits<int64_t>::min(),
                                     int64_t maximum = std::numeric_limits<int64_t>::max()) {
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<int64_t>(fmt::format("{} is required", fieldPath));
    }
    int64_t value = 0;
    if (iterator->is_number_unsigned()) {
        const auto unsignedValue = iterator->get<uint64_t>();
        if (unsignedValue > static_cast<uint64_t>(maximum)) {
            return invalid<int64_t>(fmt::format("{} exceeds maximum {}", fieldPath, maximum));
        }
        value = static_cast<int64_t>(unsignedValue);
    } else if (iterator->is_number_integer()) {
        value = iterator->get<int64_t>();
    } else {
        return invalid<int64_t>(fmt::format("{} must be an integer", fieldPath));
    }
    if (value < minimum || value > maximum) {
        return invalid<int64_t>(fmt::format("{} must be between {} and {}", fieldPath, minimum, maximum));
    }
    return Result<int64_t>{value};
}

inline Result<std::optional<int64_t>> optionalInt64(const nlohmann::json &json, std::string_view path,
                                                    std::string_view key, int64_t minimum = 0,
                                                    int64_t maximum = std::numeric_limits<int64_t>::max(),
                                                    bool allowNull = false) {
    const auto iterator = json.find(key);
    if (iterator == json.end() || (allowNull && iterator->is_null())) {
        return Result<std::optional<int64_t>>{std::optional<int64_t>{}};
    }
    auto valueResult = requiredInt64(json, path, key, minimum, maximum);
    if (!valueResult.isSuccess()) {
        return Result<std::optional<int64_t>>{valueResult.getError().value()};
    }
    return Result<std::optional<int64_t>>{std::optional<int64_t>{valueResult.getValue().value()}};
}

inline Result<double> requiredFiniteDouble(const nlohmann::json &json, std::string_view path, std::string_view key,
                                           double minimum = -std::numeric_limits<double>::max(),
                                           double maximum = std::numeric_limits<double>::max()) {
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<double>(fmt::format("{} is required", fieldPath));
    }
    if (!iterator->is_number()) {
        return invalid<double>(fmt::format("{} must be a number", fieldPath));
    }
    const auto value = iterator->get<double>();
    if (!std::isfinite(value) || value < minimum || value > maximum) {
        return invalid<double>(
            fmt::format("{} must be a finite number between {} and {}", fieldPath, minimum, maximum));
    }
    return Result<double>{value};
}

inline Result<std::optional<double>> optionalFiniteDouble(const nlohmann::json &json, std::string_view path,
                                                          std::string_view key,
                                                          double minimum = -std::numeric_limits<double>::max(),
                                                          double maximum = std::numeric_limits<double>::max(),
                                                          bool allowNull = false) {
    const auto iterator = json.find(key);
    if (iterator == json.end() || (allowNull && iterator->is_null())) {
        return Result<std::optional<double>>{std::optional<double>{}};
    }
    auto valueResult = requiredFiniteDouble(json, path, key, minimum, maximum);
    if (!valueResult.isSuccess()) {
        return Result<std::optional<double>>{valueResult.getError().value()};
    }
    return Result<std::optional<double>>{std::optional<double>{valueResult.getValue().value()}};
}

inline Result<bool> requiredBool(const nlohmann::json &json, std::string_view path, std::string_view key) {
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<bool>(fmt::format("{} is required", fieldPath));
    }
    if (!iterator->is_boolean()) {
        return invalid<bool>(fmt::format("{} must be a boolean", fieldPath));
    }
    return Result<bool>{iterator->get<bool>()};
}

inline Result<std::reference_wrapper<const nlohmann::json>> requiredArray(const nlohmann::json &json,
                                                                          std::string_view path, std::string_view key,
                                                                          std::size_t maximumEntries,
                                                                          std::size_t minimumEntries = 0) {
    const auto iterator = json.find(key);
    const auto fieldPath = fmt::format("{}.{}", path, key);
    if (iterator == json.end()) {
        return invalid<std::reference_wrapper<const nlohmann::json>>(fmt::format("{} is required", fieldPath));
    }
    if (!iterator->is_array()) {
        return invalid<std::reference_wrapper<const nlohmann::json>>(fmt::format("{} must be an array", fieldPath));
    }
    if (iterator->size() < minimumEntries) {
        return invalid<std::reference_wrapper<const nlohmann::json>>(fmt::format("{} must not be empty", fieldPath));
    }
    if (iterator->size() > maximumEntries) {
        return invalid<std::reference_wrapper<const nlohmann::json>>(
            fmt::format("{} has {} entries; maximum is {}", fieldPath, iterator->size(), maximumEntries));
    }
    return Result<std::reference_wrapper<const nlohmann::json>>{std::cref(*iterator)};
}

} // namespace creatures::json_codec
