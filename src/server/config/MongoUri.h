#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace creatures::mongo {

inline constexpr std::int32_t SERVER_SELECTION_TIMEOUT_MS = 750;
inline constexpr std::int32_t CONNECT_TIMEOUT_MS = 500;
inline constexpr std::int32_t SOCKET_TIMEOUT_MS = 1000;
inline constexpr std::int32_t WAIT_QUEUE_TIMEOUT_MS = 250;
inline constexpr std::int32_t OPERATION_MAX_TIME_MS = 750;
inline constexpr std::int32_t WRITE_CONCERN_TIMEOUT_MS = 750;

template <typename Options> void applyOperationDeadline(Options &options) {
    options.max_time(std::chrono::milliseconds(OPERATION_MAX_TIME_MS));
}

/**
 * Adds the finite MongoDB transport deadlines required by the server. Existing
 * positive values are preserved when they are lower than the server policy and
 * clamped when they are higher. Invalid, zero, and duplicate values cannot
 * disable the policy.
 */
std::string normalizeUri(const std::string &uri);

/** Remove credentials before putting a MongoDB URI in logs. */
std::string redactUri(const std::string &uri);

} // namespace creatures::mongo
