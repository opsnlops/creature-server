#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/JsonCodec.h"
#include "server/voice/VoiceClient.h"
#include "util/Result.h"
#include "util/helpers.h"

namespace creatures::api {

inline constexpr std::size_t MAX_VOICE_REQUEST_BYTES = 64 * 1024;
inline constexpr std::size_t MAX_VOICE_TEXT_BYTES = 32 * 1024;

struct MakeSoundFileRequest {
    std::string creatureId;
    std::optional<std::string> title;
    std::string text;
};

struct SpeechToTextResponse {
    std::string status;
    std::string transcript;
    double audioDurationSec;
    double transcriptionTimeMs;
};

inline Result<MakeSoundFileRequest> makeSoundFileRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "voice file request", {"creature_id", "title", "text"});
    if (!fields.isSuccess())
        return Result<MakeSoundFileRequest>{fields.getError().value()};
    auto creatureId = json_codec::requiredString(json, "voice file request", "creature_id", 64);
    auto title = json_codec::optionalString(json, "voice file request", "title", 256, true, true);
    auto text = json_codec::requiredString(json, "voice file request", "text", MAX_VOICE_TEXT_BYTES);
    if (!creatureId.isSuccess())
        return Result<MakeSoundFileRequest>{creatureId.getError().value()};
    if (!title.isSuccess())
        return Result<MakeSoundFileRequest>{title.getError().value()};
    if (!text.isSuccess())
        return Result<MakeSoundFileRequest>{text.getError().value()};
    if (!isUuidShape(creatureId.getValue().value()))
        return json_codec::invalid<MakeSoundFileRequest>("voice file request.creature_id must be a UUID");
    return Result<MakeSoundFileRequest>{
        {creatureId.getValue().value(), title.getValue().value(), text.getValue().value()}};
}

inline Result<MakeSoundFileRequest> makeSoundFileRequestFromJson(std::string_view bytes) {
    if (bytes.size() > MAX_VOICE_REQUEST_BYTES)
        return json_codec::invalid<MakeSoundFileRequest>("voice file request exceeds maximum body size");
    try {
        return makeSoundFileRequestFromJson(nlohmann::json::parse(bytes));
    } catch (const nlohmann::json::exception &error) {
        return json_codec::invalid<MakeSoundFileRequest>(
            fmt::format("invalid voice file request JSON: {}", error.what()));
    }
}

inline nlohmann::json makeSoundFileRequestToJson(const MakeSoundFileRequest &request) {
    nlohmann::json json{{"creature_id", request.creatureId}, {"text", request.text}};
    if (request.title)
        json["title"] = *request.title;
    return json;
}

inline nlohmann::json voiceToJson(const voice::Voice &voice) {
    return {{"voice_id", voice.voiceId}, {"name", voice.name}};
}

inline nlohmann::json subscriptionToJson(const voice::Subscription &subscription) {
    return {{"tier", subscription.tier},
            {"status", subscription.status},
            {"character_count", subscription.character_count},
            {"character_limit", subscription.character_limit}};
}

inline nlohmann::json creatureSpeechResponseToJson(const voice::CreatureSpeechResponse &response) {
    return {{"success", response.success},
            {"sound_file_name", response.sound_file_name},
            {"transcript_file_name", response.transcript_file_name},
            {"sound_file_size", response.sound_file_size}};
}

inline nlohmann::json speechToTextResponseToJson(const SpeechToTextResponse &response) {
    return {{"status", response.status},
            {"transcript", response.transcript},
            {"audio_duration_sec", response.audioDurationSec},
            {"transcription_time_ms", response.transcriptionTimeMs}};
}

} // namespace creatures::api
