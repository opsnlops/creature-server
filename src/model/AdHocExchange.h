#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/Result.h"
#include <nlohmann/json.hpp>

namespace creatures {

// Status values for an ad-hoc exchange (issue #150). Plain strings rather than
// an enum — they go straight into Mongo and DTOs, and clients treat them as
// opaque labels.
inline constexpr const char *EXCHANGE_STATUS_STREAMING = "streaming"; // session open, sentences arriving
inline constexpr const char *EXCHANGE_STATUS_READY = "ready";         // finished, every sentence stitched
inline constexpr const char *EXCHANGE_STATUS_PARTIAL = "partial";     // finished, some sentences failed
inline constexpr const char *EXCHANGE_STATUS_FAILED = "failed";       // finished, nothing rendered

/// One sentence of a streamed ad-hoc exchange, in playback order.
struct AdHocExchangePart {
    uint32_t index{0}; // 1-based sentence index (matches the sN.wav filename)
    std::string animation_id;
    std::string text;
    uint64_t duration_ms{0};

    bool operator==(const AdHocExchangePart &) const = default;
};

/// One completed (or in-flight) streaming ad-hoc session: everything a creature
/// said in one creature-agent-driven conversation turn. The durable record the
/// exchange-export endpoints serve from (issue #150). TTL'd alongside the
/// ad-hoc animations it references.
struct AdHocExchange {
    std::string session_id; // natural key; also the session directory name
    std::string creature_id;
    std::string creature_name;
    std::string status{EXCHANGE_STATUS_STREAMING};
    std::string title;      // "<creature> - <timestamp> - <slug>"; empty until finalized
    std::string transcript; // full text, empty until finalized
    std::string sound_file; // ABSOLUTE path to the stitched WAV (AdHoc bucket rule)
    uint64_t duration_ms{0};
    int64_t finished_at_ms{0}; // epoch millis; 0 while streaming
    std::vector<AdHocExchangePart> parts;

    bool operator==(const AdHocExchange &) const = default;
};

nlohmann::json adHocExchangeToJson(const AdHocExchange &exchange);
Result<AdHocExchange> adHocExchangeFromJson(const nlohmann::json &json);

} // namespace creatures
