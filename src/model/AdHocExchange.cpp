#include "model/AdHocExchange.h"

#include <fmt/format.h>

namespace creatures {

nlohmann::json adHocExchangeToJson(const AdHocExchange &exchange) {
    nlohmann::json parts = nlohmann::json::array();
    for (const auto &part : exchange.parts) {
        parts.push_back({{"index", part.index},
                         {"animation_id", part.animation_id},
                         {"text", part.text},
                         {"duration_ms", part.duration_ms}});
    }
    return {{"session_id", exchange.session_id},
            {"creature_id", exchange.creature_id},
            {"creature_name", exchange.creature_name},
            {"status", exchange.status},
            {"title", exchange.title},
            {"transcript", exchange.transcript},
            {"sound_file", exchange.sound_file},
            {"duration_ms", exchange.duration_ms},
            {"finished_at_ms", exchange.finished_at_ms},
            {"parts", parts}};
}

Result<AdHocExchange> adHocExchangeFromJson(const nlohmann::json &json) {
    try {
        AdHocExchange exchange;
        exchange.session_id = json.at("session_id").get<std::string>();
        exchange.creature_id = json.at("creature_id").get<std::string>();
        exchange.creature_name = json.value("creature_name", "");
        exchange.status = json.value("status", std::string{EXCHANGE_STATUS_STREAMING});
        exchange.title = json.value("title", "");
        exchange.transcript = json.value("transcript", "");
        exchange.sound_file = json.value("sound_file", "");
        exchange.duration_ms = json.value("duration_ms", uint64_t{0});
        exchange.finished_at_ms = json.value("finished_at_ms", int64_t{0});
        if (json.contains("parts") && json["parts"].is_array()) {
            for (const auto &partJson : json["parts"]) {
                AdHocExchangePart part;
                part.index = partJson.value("index", uint32_t{0});
                part.animation_id = partJson.value("animation_id", "");
                part.text = partJson.value("text", "");
                part.duration_ms = partJson.value("duration_ms", uint64_t{0});
                exchange.parts.push_back(std::move(part));
            }
        }
        return exchange;
    } catch (const std::exception &e) {
        return Result<AdHocExchange>{
            ServerError(ServerError::InvalidData, fmt::format("Invalid ad-hoc exchange JSON: {}", e.what()))};
    }
}

} // namespace creatures
