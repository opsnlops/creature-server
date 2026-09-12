#include "model/AdHocExchange.h"

#include <limits>

#include <fmt/format.h>

#include "model/JsonCodec.h"
#include "util/UuidValidation.h"

namespace creatures {

nlohmann::json adHocExchangeToJson(const AdHocExchange &exchange) {
    nlohmann::json parts = nlohmann::json::array();
    for (const auto &part : exchange.parts) {
        nlohmann::json partJson = {{"index", part.index},
                                   {"animation_id", part.animation_id},
                                   {"text", part.text},
                                   {"duration_ms", part.duration_ms}};
        // Only written when known, so single-creature records keep their
        // pre-#186 shape byte for byte.
        if (!part.creature_id.empty()) {
            partJson["creature_id"] = part.creature_id;
            partJson["creature_name"] = part.creature_name;
        }
        parts.push_back(std::move(partJson));
    }
    nlohmann::json participants = nlohmann::json::array();
    for (const auto &participant : exchange.participants) {
        participants.push_back({{"creature_id", participant.creature_id},
                                {"creature_name", participant.creature_name},
                                {"audio_channel", participant.audio_channel}});
    }
    nlohmann::json json = {{"session_id", exchange.session_id},
                           {"creature_id", exchange.creature_id},
                           {"creature_name", exchange.creature_name},
                           {"status", exchange.status},
                           {"title", exchange.title},
                           {"transcript", exchange.transcript},
                           {"sound_file", exchange.sound_file},
                           {"duration_ms", exchange.duration_ms},
                           {"finished_at_ms", exchange.finished_at_ms},
                           {"parts", parts}};
    if (!exchange.participants.empty())
        json["participants"] = std::move(participants);
    if (!exchange.stage_id.empty())
        json["stage_id"] = exchange.stage_id;
    return json;
}

Result<AdHocExchange> adHocExchangeFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "ad-hoc exchange";
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"_id", "created_at", "session_id", "creature_id", "creature_name",
                                                   "status", "title", "transcript", "sound_file", "duration_ms",
                                                   "finished_at_ms", "parts", "participants", "stage_id"});
    if (!fields.isSuccess())
        return Result<AdHocExchange>{fields.getError().value()};

    auto sessionId = json_codec::requiredString(json, path, "session_id", 36);
    auto creatureId = json_codec::requiredString(json, path, "creature_id", 36);
    auto creatureName = json_codec::optionalString(json, path, "creature_name", 256, true);
    auto status = json_codec::optionalString(json, path, "status", 32);
    auto title = json_codec::optionalString(json, path, "title", 1024, true);
    auto transcript = json_codec::optionalString(json, path, "transcript", MAX_AD_HOC_EXCHANGE_TRANSCRIPT_BYTES, true);
    auto soundFile = json_codec::optionalString(json, path, "sound_file", 4096, true);
    auto duration = json_codec::optionalUnsigned<uint64_t>(json, path, "duration_ms");
    auto finishedAt = json_codec::optionalInt64(json, path, "finished_at_ms", 0);
    auto stageId = json_codec::optionalString(json, path, "stage_id", 36, true);
    if (!sessionId.isSuccess())
        return Result<AdHocExchange>{sessionId.getError().value()};
    if (!creatureId.isSuccess())
        return Result<AdHocExchange>{creatureId.getError().value()};
    if (!isUuidShape(sessionId.getValue().value()))
        return json_codec::invalid<AdHocExchange>("ad-hoc exchange.session_id must be a UUID");
    if (!isUuidShape(creatureId.getValue().value()))
        return json_codec::invalid<AdHocExchange>("ad-hoc exchange.creature_id must be a UUID");
    if (!creatureName.isSuccess())
        return Result<AdHocExchange>{creatureName.getError().value()};
    if (!status.isSuccess())
        return Result<AdHocExchange>{status.getError().value()};
    if (!title.isSuccess())
        return Result<AdHocExchange>{title.getError().value()};
    if (!transcript.isSuccess())
        return Result<AdHocExchange>{transcript.getError().value()};
    if (!soundFile.isSuccess())
        return Result<AdHocExchange>{soundFile.getError().value()};
    if (!duration.isSuccess())
        return Result<AdHocExchange>{duration.getError().value()};
    if (!finishedAt.isSuccess())
        return Result<AdHocExchange>{finishedAt.getError().value()};
    if (!stageId.isSuccess())
        return Result<AdHocExchange>{stageId.getError().value()};
    if (const auto stageValue = stageId.getValue()->value_or(""); !stageValue.empty() && !isUuidShape(stageValue))
        return json_codec::invalid<AdHocExchange>("ad-hoc exchange.stage_id must be a UUID");

    const auto statusValue = status.getValue()->value_or(EXCHANGE_STATUS_STREAMING);
    if (statusValue != EXCHANGE_STATUS_STREAMING && statusValue != EXCHANGE_STATUS_READY &&
        statusValue != EXCHANGE_STATUS_PARTIAL && statusValue != EXCHANGE_STATUS_FAILED) {
        return json_codec::invalid<AdHocExchange>(
            fmt::format("ad-hoc exchange.status has unknown value '{}'", statusValue));
    }

    AdHocExchange exchange;
    exchange.session_id = sessionId.getValue().value();
    exchange.creature_id = creatureId.getValue().value();
    exchange.creature_name = creatureName.getValue()->value_or("");
    exchange.status = statusValue;
    exchange.title = title.getValue()->value_or("");
    exchange.transcript = transcript.getValue()->value_or("");
    exchange.sound_file = soundFile.getValue()->value_or("");
    exchange.duration_ms = duration.getValue()->value_or(0);
    exchange.finished_at_ms = finishedAt.getValue()->value_or(0);
    exchange.stage_id = stageId.getValue()->value_or("");

    if (json.contains("participants")) {
        auto participantsResult =
            json_codec::requiredArray(json, path, "participants", MAX_AD_HOC_EXCHANGE_PARTICIPANTS);
        if (!participantsResult.isSuccess())
            return Result<AdHocExchange>{participantsResult.getError().value()};
        const auto &participants = participantsResult.getValue()->get();
        exchange.participants.reserve(participants.size());
        for (std::size_t index = 0; index < participants.size(); ++index) {
            const auto participantPath = fmt::format("ad-hoc exchange.participants[{}]", index);
            const auto &participantJson = participants[index];
            auto participantFields = json_codec::rejectUnknownFields(participantJson, participantPath,
                                                                     {"creature_id", "creature_name", "audio_channel"});
            if (!participantFields.isSuccess())
                return Result<AdHocExchange>{participantFields.getError().value()};
            auto participantId = json_codec::requiredString(participantJson, participantPath, "creature_id", 36);
            auto participantName =
                json_codec::optionalString(participantJson, participantPath, "creature_name", 256, true);
            auto channel = json_codec::requiredUnsigned<uint16_t>(participantJson, participantPath, "audio_channel",
                                                                  std::numeric_limits<uint16_t>::max());
            if (!participantId.isSuccess())
                return Result<AdHocExchange>{participantId.getError().value()};
            if (!isUuidShape(participantId.getValue().value()))
                return json_codec::invalid<AdHocExchange>(
                    fmt::format("{}.creature_id must be a UUID", participantPath));
            if (!participantName.isSuccess())
                return Result<AdHocExchange>{participantName.getError().value()};
            if (!channel.isSuccess())
                return Result<AdHocExchange>{channel.getError().value()};
            exchange.participants.push_back({participantId.getValue().value(), participantName.getValue()->value_or(""),
                                             channel.getValue().value()});
        }
    }

    const auto partsIterator = json.find("parts");
    if (partsIterator == json.end())
        return Result<AdHocExchange>{std::move(exchange)};
    auto partsResult = json_codec::requiredArray(json, path, "parts", MAX_AD_HOC_EXCHANGE_PARTS);
    if (!partsResult.isSuccess())
        return Result<AdHocExchange>{partsResult.getError().value()};

    const auto &parts = partsResult.getValue()->get();
    exchange.parts.reserve(parts.size());
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const auto partPath = fmt::format("ad-hoc exchange.parts[{}]", index);
        const auto &partJson = parts[index];
        auto partFields = json_codec::rejectUnknownFields(
            partJson, partPath, {"index", "animation_id", "text", "duration_ms", "creature_id", "creature_name"});
        if (!partFields.isSuccess())
            return Result<AdHocExchange>{partFields.getError().value()};
        auto partIndex =
            json_codec::requiredUnsigned<uint32_t>(partJson, partPath, "index", std::numeric_limits<uint32_t>::max());
        auto animationId = json_codec::requiredString(partJson, partPath, "animation_id", 36);
        auto text = json_codec::requiredString(partJson, partPath, "text", MAX_AD_HOC_EXCHANGE_TEXT_BYTES);
        auto partDuration = json_codec::requiredUnsigned<uint64_t>(partJson, partPath, "duration_ms",
                                                                   std::numeric_limits<uint64_t>::max());
        auto partCreatureId = json_codec::optionalString(partJson, partPath, "creature_id", 36, true);
        auto partCreatureName = json_codec::optionalString(partJson, partPath, "creature_name", 256, true);
        if (!partIndex.isSuccess())
            return Result<AdHocExchange>{partIndex.getError().value()};
        if (!animationId.isSuccess())
            return Result<AdHocExchange>{animationId.getError().value()};
        if (!isUuidShape(animationId.getValue().value()))
            return json_codec::invalid<AdHocExchange>(fmt::format("{}.animation_id must be a UUID", partPath));
        if (!text.isSuccess())
            return Result<AdHocExchange>{text.getError().value()};
        if (!partDuration.isSuccess())
            return Result<AdHocExchange>{partDuration.getError().value()};
        if (!partCreatureId.isSuccess())
            return Result<AdHocExchange>{partCreatureId.getError().value()};
        if (!partCreatureName.isSuccess())
            return Result<AdHocExchange>{partCreatureName.getError().value()};
        const auto partCreatureIdValue = partCreatureId.getValue()->value_or("");
        if (!partCreatureIdValue.empty() && !isUuidShape(partCreatureIdValue))
            return json_codec::invalid<AdHocExchange>(fmt::format("{}.creature_id must be a UUID", partPath));
        exchange.parts.push_back({partIndex.getValue().value(), animationId.getValue().value(), text.getValue().value(),
                                  partDuration.getValue().value(), partCreatureIdValue,
                                  partCreatureName.getValue()->value_or("")});
    }
    return Result<AdHocExchange>{std::move(exchange)};
}

} // namespace creatures
