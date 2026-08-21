#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>
#include <oatpp/core/macro/codegen.hpp>

#include "util/Result.h"

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

#include OATPP_CODEGEN_BEGIN(DTO)

class AdHocExchangePartDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangePartDto, DTO)

    DTO_FIELD_INFO(index) { info->description = "1-based sentence index, in playback order"; }
    DTO_FIELD(UInt32, index);

    DTO_FIELD_INFO(animation_id) { info->description = "The ad-hoc animation this sentence played as"; }
    DTO_FIELD(String, animation_id);

    DTO_FIELD_INFO(text) { info->description = "The sentence's text"; }
    DTO_FIELD(String, text);

    DTO_FIELD_INFO(duration_ms) { info->description = "Sentence audio duration in milliseconds"; }
    DTO_FIELD(UInt64, duration_ms);
};

class AdHocExchangeDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangeDto, DTO)

    DTO_FIELD_INFO(session_id) { info->description = "The streaming session this exchange records"; }
    DTO_FIELD(String, session_id);

    DTO_FIELD_INFO(creature_id) { info->description = "The creature that spoke"; }
    DTO_FIELD(String, creature_id);

    DTO_FIELD_INFO(creature_name) { info->description = "The creature's display name"; }
    DTO_FIELD(String, creature_name);

    DTO_FIELD_INFO(status) { info->description = "streaming | ready | partial | failed"; }
    DTO_FIELD(String, status);

    DTO_FIELD_INFO(title) { info->description = "Display title; empty until the session finishes"; }
    DTO_FIELD(String, title);

    DTO_FIELD_INFO(transcript) { info->description = "Full transcript; empty until the session finishes"; }
    DTO_FIELD(String, transcript);

    DTO_FIELD_INFO(duration_ms) { info->description = "Stitched audio duration in milliseconds"; }
    DTO_FIELD(UInt64, duration_ms);

    DTO_FIELD_INFO(created_at) { info->description = "Session start timestamp in ISO8601 format"; }
    DTO_FIELD(String, created_at);

    DTO_FIELD_INFO(finished_at) {
        info->description = "Session finish timestamp in ISO8601 format; absent while streaming";
    }
    DTO_FIELD(String, finished_at);

    DTO_FIELD_INFO(parts) { info->description = "The sentences of the exchange, in playback order"; }
    DTO_FIELD(Vector<Object<AdHocExchangePartDto>>, parts);
};

class AdHocExchangeListDto : public oatpp::DTO {
    DTO_INIT(AdHocExchangeListDto, DTO)

    DTO_FIELD_INFO(count) { info->description = "Number of exchanges returned"; }
    DTO_FIELD(UInt32, count);

    DTO_FIELD_INFO(items) { info->description = "Exchanges, newest first"; }
    DTO_FIELD(Vector<Object<AdHocExchangeDto>>, items);
};

#include OATPP_CODEGEN_END(DTO)

oatpp::Object<AdHocExchangeDto> convertToDto(const AdHocExchange &exchange,
                                             const std::chrono::system_clock::time_point &createdAt);

} // namespace creatures
