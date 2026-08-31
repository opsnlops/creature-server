#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "model/DialogScriptTypes.h"
#include "model/JsonCodec.h"
#include "server/voice/DialogClient.h"
#include "server/voice/MusicClient.h"
#include "util/Result.h"
#include "util/helpers.h"

namespace creatures::api {

inline constexpr std::size_t MAX_DIALOG_REQUEST_BYTES = 1024 * 1024;
inline constexpr std::size_t MAX_DIALOG_CACHE_KEY_BYTES = 64;
inline constexpr std::size_t MAX_DIALOG_GENERATION_ID_BYTES = 64;

struct DialogTurnRequest {
    std::string creatureId;
    std::string text;
};

struct DialogRequest {
    std::vector<DialogTurnRequest> turns;
    std::optional<std::string> scriptId;
    std::string persistence;
    bool autoplay = false;
    std::optional<std::string> title;
    std::optional<std::string> stageId;
    std::optional<std::string> generationId;
};

struct DialogPreviewRequest {
    std::vector<DialogTurnRequest> turns;
    std::optional<std::string> generationId;
    bool regenerate = false;
    std::optional<std::string> title;
};

struct DialogMusicRequest {
    std::string scriptId;
    std::string dialogCacheKey;
    std::string dialogGenerationId;
    std::string prompt;
    int64_t durationExtensionMs = 0;
    std::string generationMode = "track";
};

struct AcceptVoiceTakeRequest {
    std::string scriptId;
    std::string generationId;
    std::string dialogCacheKey;
};

struct DialogMusicGenerationResult {
    std::string musicGenerationId;
    std::string mp3Url;
    double durationSeconds = 0;
    int64_t dialogDurationMs = 0;
    int64_t durationExtensionMs = 0;
    int64_t requestedMusicLengthMs = 0;
    std::string prompt;
};

struct DialogMusicPromotionResult {
    std::string musicGenerationId;
    std::string soundFile;
    std::string mp3Url;
};

struct DialogJobResult {
    std::string animationId;
    uint32_t numberOfFrames = 0;
    uint32_t millisecondsPerFrame = 0;
    double durationSeconds = 0;
    std::string persistence;
    bool autoplayed = false;
};

struct DialogPreviewExportResult {
    std::string fileName;
    std::string generationId;
    std::string cacheKey;
};

struct DialogPreviewVoiceSegment {
    std::string voiceId;
    uint64_t characterStartIndex = 0;
    uint64_t characterEndIndex = 0;
    uint64_t dialogInputIndex = 0;
};

struct DialogPreviewTiming {
    std::string text;
    double start = 0;
    double end = 0;
};

struct DialogPreviewMetaResponse {
    std::string cacheKey;
    std::string generationId;
    bool cached = false;
    std::string audioUrl;
    std::string audioFormat;
    uint32_t sampleRate = 0;
    double durationSeconds = 0;
    std::vector<DialogPreviewVoiceSegment> voiceSegments;
    std::vector<DialogPreviewTiming> forcedAlignmentWords;
    std::vector<DialogPreviewTiming> forcedAlignmentChars;
    double forcedAlignmentLoss = 0;
};

struct DialogPreviewGenerationEntry {
    std::string generationId;
    std::string createdAt;
};

struct DialogPreviewLookupResponse {
    std::string cacheKey;
    std::vector<DialogPreviewGenerationEntry> generations;
    std::string latestGenerationId;
};

struct DialogScriptValidationResponse {
    bool valid = true;
    std::optional<std::string> scriptId;
    uint32_t turnCount = 0;
    std::vector<std::string> missingCreatureIds;
    std::vector<std::string> errorMessages;
};

inline bool isLowercaseSha256(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}

inline Result<void> validateDialogPreviewRequest(const DialogPreviewRequest &request) {
    if (request.turns.empty() || request.turns.size() > MAX_DIALOG_SCRIPT_TURNS)
        return json_codec::invalid<void>(
            fmt::format("dialog preview request.turns must contain between 1 and {} entries", MAX_DIALOG_SCRIPT_TURNS));
    std::size_t totalBytes = 0;
    for (std::size_t index = 0; index < request.turns.size(); ++index) {
        const auto &turn = request.turns[index];
        if (!isUuidShape(turn.creatureId))
            return json_codec::invalid<void>(
                fmt::format("dialog preview request.turns[{}].creature_id must be a UUID", index));
        if (turn.text.empty() || turn.text.size() > MAX_DIALOG_SCRIPT_TURN_TEXT)
            return json_codec::invalid<void>(
                fmt::format("dialog preview request.turns[{}].text must contain between 1 and {} bytes", index,
                            MAX_DIALOG_SCRIPT_TURN_TEXT));
        totalBytes += turn.creatureId.size() + turn.text.size();
        if (totalBytes > MAX_DIALOG_REQUEST_BYTES)
            return json_codec::invalid<void>("dialog preview request exceeds maximum body size");
    }
    if (request.generationId && !isUuidShape(*request.generationId))
        return json_codec::invalid<void>("dialog preview request.generation_id must be a UUID");
    if (request.title && request.title->size() > MAX_DIALOG_SCRIPT_TITLE)
        return json_codec::invalid<void>("dialog preview request.title exceeds maximum length");
    return Result<void>{};
}

inline Result<nlohmann::json> parseContractJson(std::string_view bytes, std::string_view contractName) {
    if (bytes.size() > MAX_DIALOG_REQUEST_BYTES) {
        return Result<nlohmann::json>{
            ServerError(ServerError::InvalidData, fmt::format("{} is {} bytes; maximum is {}", contractName,
                                                              bytes.size(), MAX_DIALOG_REQUEST_BYTES))};
    }
    try {
        return Result<nlohmann::json>{nlohmann::json::parse(bytes)};
    } catch (const nlohmann::json::exception &error) {
        return Result<nlohmann::json>{
            ServerError(ServerError::InvalidData, fmt::format("invalid {} JSON: {}", contractName, error.what()))};
    }
}

inline Result<std::optional<bool>> optionalBool(const nlohmann::json &json, std::string_view path, std::string_view key,
                                                bool allowNull = true) {
    const auto iterator = json.find(key);
    if (iterator == json.end() || (allowNull && iterator->is_null()))
        return Result<std::optional<bool>>{std::optional<bool>{}};
    auto value = json_codec::requiredBool(json, path, key);
    if (!value.isSuccess())
        return Result<std::optional<bool>>{value.getError().value()};
    return Result<std::optional<bool>>{std::optional<bool>{value.getValue().value()}};
}

inline Result<std::vector<DialogTurnRequest>> dialogTurnsFromJson(const nlohmann::json &json, std::string_view path) {
    if (!json.is_array())
        return json_codec::invalid<std::vector<DialogTurnRequest>>(fmt::format("{} must be an array", path));
    if (json.empty() || json.size() > MAX_DIALOG_SCRIPT_TURNS) {
        return json_codec::invalid<std::vector<DialogTurnRequest>>(
            fmt::format("{} must contain between 1 and {} entries", path, MAX_DIALOG_SCRIPT_TURNS));
    }
    std::vector<DialogTurnRequest> turns;
    turns.reserve(json.size());
    for (std::size_t index = 0; index < json.size(); ++index) {
        const auto itemPath = fmt::format("{}[{}]", path, index);
        auto fields = json_codec::rejectUnknownFields(json[index], itemPath, {"creature_id", "text"});
        if (!fields.isSuccess())
            return Result<std::vector<DialogTurnRequest>>{fields.getError().value()};
        auto creatureId = json_codec::requiredString(json[index], itemPath, "creature_id", 64);
        if (!creatureId.isSuccess())
            return Result<std::vector<DialogTurnRequest>>{creatureId.getError().value()};
        if (!isUuidShape(creatureId.getValue().value()))
            return json_codec::invalid<std::vector<DialogTurnRequest>>(
                fmt::format("{}.creature_id must be a UUID", itemPath));
        auto text = json_codec::requiredString(json[index], itemPath, "text", MAX_DIALOG_SCRIPT_TURN_TEXT);
        if (!text.isSuccess())
            return Result<std::vector<DialogTurnRequest>>{text.getError().value()};
        turns.push_back({creatureId.getValue().value(), text.getValue().value()});
    }
    return Result<std::vector<DialogTurnRequest>>{std::move(turns)};
}

inline Result<DialogRequest> dialogRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(
        json, "dialog request",
        {"turns", "script_id", "persistence", "autoplay", "title", "stage_id", "generation_id"});
    if (!fields.isSuccess())
        return Result<DialogRequest>{fields.getError().value()};
    DialogRequest request;
    if (const auto turns = json.find("turns"); turns != json.end() && !turns->is_null()) {
        auto parsed = dialogTurnsFromJson(*turns, "dialog request.turns");
        if (!parsed.isSuccess())
            return Result<DialogRequest>{parsed.getError().value()};
        request.turns = parsed.getValue().value();
    }
    auto scriptId = json_codec::optionalString(json, "dialog request", "script_id", 64, false, true);
    auto persistence = json_codec::requiredString(json, "dialog request", "persistence", 16);
    auto autoplay = optionalBool(json, "dialog request", "autoplay");
    auto title = json_codec::optionalString(json, "dialog request", "title", MAX_DIALOG_SCRIPT_TITLE, true, true);
    auto stageId = json_codec::optionalString(json, "dialog request", "stage_id", 64, true, true);
    auto generationId = json_codec::optionalString(json, "dialog request", "generation_id", 64, false, true);
    if (!scriptId.isSuccess())
        return Result<DialogRequest>{scriptId.getError().value()};
    if (!persistence.isSuccess())
        return Result<DialogRequest>{persistence.getError().value()};
    if (!autoplay.isSuccess())
        return Result<DialogRequest>{autoplay.getError().value()};
    if (!title.isSuccess())
        return Result<DialogRequest>{title.getError().value()};
    if (!stageId.isSuccess())
        return Result<DialogRequest>{stageId.getError().value()};
    if (!generationId.isSuccess())
        return Result<DialogRequest>{generationId.getError().value()};
    request.scriptId = scriptId.getValue().value();
    request.persistence = persistence.getValue().value();
    request.autoplay = autoplay.getValue().value().value_or(false);
    request.title = title.getValue().value();
    request.stageId = stageId.getValue().value();
    request.generationId = generationId.getValue().value();
    const bool hasTurns = !request.turns.empty();
    const bool hasScriptId = request.scriptId && !request.scriptId->empty();
    if (hasTurns == hasScriptId)
        return json_codec::invalid<DialogRequest>("dialog request requires exactly one of turns or script_id");
    if (hasScriptId && !isUuidShape(*request.scriptId))
        return json_codec::invalid<DialogRequest>("dialog request.script_id must be a UUID");
    if (request.stageId && !request.stageId->empty() && !isUuidShape(*request.stageId))
        return json_codec::invalid<DialogRequest>("dialog request.stage_id must be a UUID");
    if (request.generationId && !isUuidShape(*request.generationId))
        return json_codec::invalid<DialogRequest>("dialog request.generation_id must be a UUID");
    if (request.persistence != "adhoc" && request.persistence != "permanent")
        return json_codec::invalid<DialogRequest>("dialog request.persistence must be 'adhoc' or 'permanent'");
    return Result<DialogRequest>{std::move(request)};
}

inline Result<DialogPreviewRequest> dialogPreviewRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "dialog preview request",
                                                  {"turns", "generation_id", "regenerate", "title"});
    if (!fields.isSuccess())
        return Result<DialogPreviewRequest>{fields.getError().value()};
    auto turnsJson = json_codec::requiredArray(json, "dialog preview request", "turns", MAX_DIALOG_SCRIPT_TURNS, 1);
    if (!turnsJson.isSuccess())
        return Result<DialogPreviewRequest>{turnsJson.getError().value()};
    auto turns = dialogTurnsFromJson(turnsJson.getValue().value().get(), "dialog preview request.turns");
    auto generationId = json_codec::optionalString(json, "dialog preview request", "generation_id", 64, false, true);
    auto regenerate = optionalBool(json, "dialog preview request", "regenerate");
    auto title =
        json_codec::optionalString(json, "dialog preview request", "title", MAX_DIALOG_SCRIPT_TITLE, true, true);
    if (!turns.isSuccess())
        return Result<DialogPreviewRequest>{turns.getError().value()};
    if (!generationId.isSuccess())
        return Result<DialogPreviewRequest>{generationId.getError().value()};
    if (!regenerate.isSuccess())
        return Result<DialogPreviewRequest>{regenerate.getError().value()};
    if (!title.isSuccess())
        return Result<DialogPreviewRequest>{title.getError().value()};
    DialogPreviewRequest request{turns.getValue().value(), generationId.getValue().value(),
                                 regenerate.getValue().value().value_or(false), title.getValue().value()};
    auto validation = validateDialogPreviewRequest(request);
    if (!validation.isSuccess())
        return Result<DialogPreviewRequest>{validation.getError().value()};
    return Result<DialogPreviewRequest>{std::move(request)};
}

inline Result<DialogMusicRequest> dialogMusicRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "dialog music request",
                                                  {"script_id", "dialog_cache_key", "dialog_generation_id", "prompt",
                                                   "duration_extension_ms", "generation_mode"});
    if (!fields.isSuccess())
        return Result<DialogMusicRequest>{fields.getError().value()};
    auto scriptId = json_codec::requiredString(json, "dialog music request", "script_id", 64);
    auto cacheKey = json_codec::requiredString(json, "dialog music request", "dialog_cache_key", 64);
    auto generationId = json_codec::requiredString(json, "dialog music request", "dialog_generation_id", 64);
    auto prompt = json_codec::requiredString(json, "dialog music request", "prompt", voice::kMaxMusicPromptBytes);
    auto duration = json_codec::optionalInt64(json, "dialog music request", "duration_extension_ms", 0,
                                              voice::kMaxMusicDurationExtensionMs, true);
    auto mode = json_codec::optionalString(json, "dialog music request", "generation_mode", 16, false, true);
    if (!scriptId.isSuccess())
        return Result<DialogMusicRequest>{scriptId.getError().value()};
    if (!cacheKey.isSuccess())
        return Result<DialogMusicRequest>{cacheKey.getError().value()};
    if (!generationId.isSuccess())
        return Result<DialogMusicRequest>{generationId.getError().value()};
    if (!prompt.isSuccess())
        return Result<DialogMusicRequest>{prompt.getError().value()};
    if (!duration.isSuccess())
        return Result<DialogMusicRequest>{duration.getError().value()};
    if (!mode.isSuccess())
        return Result<DialogMusicRequest>{mode.getError().value()};
    DialogMusicRequest request{scriptId.getValue().value(),
                               cacheKey.getValue().value(),
                               generationId.getValue().value(),
                               prompt.getValue().value(),
                               duration.getValue().value().value_or(0),
                               mode.getValue().value().value_or("track")};
    if (!isUuidShape(request.scriptId))
        return json_codec::invalid<DialogMusicRequest>("dialog music request.script_id must be a UUID");
    if (!isLowercaseSha256(request.dialogCacheKey))
        return json_codec::invalid<DialogMusicRequest>(
            "dialog music request.dialog_cache_key must be a 64-character lowercase hex sha256");
    if (!isUuidShape(request.dialogGenerationId))
        return json_codec::invalid<DialogMusicRequest>("dialog music request.dialog_generation_id must be a UUID");
    if (request.generationMode != "track" && request.generationMode != "loop" && request.generationMode != "ambience")
        return json_codec::invalid<DialogMusicRequest>(
            "dialog music request.generation_mode must be 'track', 'loop', or 'ambience'");
    return Result<DialogMusicRequest>{std::move(request)};
}

inline Result<AcceptVoiceTakeRequest> acceptVoiceTakeRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "accept voice take request",
                                                  {"script_id", "generation_id", "dialog_cache_key"});
    if (!fields.isSuccess())
        return Result<AcceptVoiceTakeRequest>{fields.getError().value()};
    auto scriptId = json_codec::requiredString(json, "accept voice take request", "script_id", 64);
    auto generationId = json_codec::requiredString(json, "accept voice take request", "generation_id", 64);
    auto cacheKey = json_codec::requiredString(json, "accept voice take request", "dialog_cache_key", 64);
    if (!scriptId.isSuccess())
        return Result<AcceptVoiceTakeRequest>{scriptId.getError().value()};
    if (!generationId.isSuccess())
        return Result<AcceptVoiceTakeRequest>{generationId.getError().value()};
    if (!cacheKey.isSuccess())
        return Result<AcceptVoiceTakeRequest>{cacheKey.getError().value()};
    AcceptVoiceTakeRequest request{scriptId.getValue().value(), generationId.getValue().value(),
                                   cacheKey.getValue().value()};
    if (!isUuidShape(request.scriptId))
        return json_codec::invalid<AcceptVoiceTakeRequest>("accept voice take request.script_id must be a UUID");
    if (!isUuidShape(request.generationId))
        return json_codec::invalid<AcceptVoiceTakeRequest>("accept voice take request.generation_id must be a UUID");
    if (!isLowercaseSha256(request.dialogCacheKey))
        return json_codec::invalid<AcceptVoiceTakeRequest>(
            "accept voice take request.dialog_cache_key must be a 64-character lowercase hex sha256");
    return Result<AcceptVoiceTakeRequest>{std::move(request)};
}

inline Result<std::vector<DialogTurnRequest>> dialogPreviewLookupRequestFromJson(const nlohmann::json &json) {
    auto fields = json_codec::rejectUnknownFields(json, "dialog preview lookup request", {"turns"});
    if (!fields.isSuccess())
        return Result<std::vector<DialogTurnRequest>>{fields.getError().value()};
    auto turnsJson =
        json_codec::requiredArray(json, "dialog preview lookup request", "turns", MAX_DIALOG_SCRIPT_TURNS, 1);
    if (!turnsJson.isSuccess())
        return Result<std::vector<DialogTurnRequest>>{turnsJson.getError().value()};
    return dialogTurnsFromJson(turnsJson.getValue().value().get(), "dialog preview lookup request.turns");
}

inline nlohmann::json dialogTurnsToJson(const std::vector<DialogTurnRequest> &turns) {
    auto json = nlohmann::json::array();
    for (const auto &turn : turns)
        json.push_back({{"creature_id", turn.creatureId}, {"text", turn.text}});
    return json;
}

inline nlohmann::json dialogRequestToJson(const DialogRequest &request) {
    nlohmann::json json{{"persistence", request.persistence}, {"autoplay", request.autoplay}};
    if (!request.turns.empty())
        json["turns"] = dialogTurnsToJson(request.turns);
    if (request.scriptId)
        json["script_id"] = *request.scriptId;
    if (request.title)
        json["title"] = *request.title;
    if (request.stageId)
        json["stage_id"] = *request.stageId;
    if (request.generationId)
        json["generation_id"] = *request.generationId;
    return json;
}

inline nlohmann::json dialogPreviewRequestToJson(const DialogPreviewRequest &request) {
    nlohmann::json json{{"turns", dialogTurnsToJson(request.turns)}, {"regenerate", request.regenerate}};
    if (request.generationId)
        json["generation_id"] = *request.generationId;
    if (request.title)
        json["title"] = *request.title;
    return json;
}

inline nlohmann::json dialogMusicRequestToJson(const DialogMusicRequest &request) {
    return {{"script_id", request.scriptId},
            {"dialog_cache_key", request.dialogCacheKey},
            {"dialog_generation_id", request.dialogGenerationId},
            {"prompt", request.prompt},
            {"duration_extension_ms", request.durationExtensionMs},
            {"generation_mode", request.generationMode}};
}

inline nlohmann::json dialogMusicGenerationResultToJson(const DialogMusicGenerationResult &result) {
    return {{"music_generation_id", result.musicGenerationId},
            {"mp3_url", result.mp3Url},
            {"duration_seconds", result.durationSeconds},
            {"dialog_duration_ms", result.dialogDurationMs},
            {"duration_extension_ms", result.durationExtensionMs},
            {"requested_music_length_ms", result.requestedMusicLengthMs},
            {"prompt", result.prompt}};
}

inline nlohmann::json dialogMusicPromotionResultToJson(const DialogMusicPromotionResult &result) {
    return {{"music_generation_id", result.musicGenerationId},
            {"sound_file", result.soundFile},
            {"mp3_url", result.mp3Url}};
}

inline nlohmann::json dialogJobResultToJson(const DialogJobResult &result) {
    return {{"animation_id", result.animationId},
            {"number_of_frames", result.numberOfFrames},
            {"milliseconds_per_frame", result.millisecondsPerFrame},
            {"duration_seconds", result.durationSeconds},
            {"persistence", result.persistence},
            {"autoplayed", result.autoplayed}};
}

inline nlohmann::json dialogPreviewExportResultToJson(const DialogPreviewExportResult &result) {
    return {{"file_name", result.fileName}, {"generation_id", result.generationId}, {"cache_key", result.cacheKey}};
}

inline nlohmann::json dialogPreviewMetaResponseToJson(const DialogPreviewMetaResponse &response) {
    auto segments = nlohmann::json::array();
    for (const auto &segment : response.voiceSegments) {
        segments.push_back({{"voice_id", segment.voiceId},
                            {"character_start_index", segment.characterStartIndex},
                            {"character_end_index", segment.characterEndIndex},
                            {"dialog_input_index", segment.dialogInputIndex}});
    }
    const auto timingsToJson = [](const std::vector<DialogPreviewTiming> &timings) {
        auto json = nlohmann::json::array();
        for (const auto &timing : timings)
            json.push_back({{"text", timing.text}, {"start", timing.start}, {"end", timing.end}});
        return json;
    };
    return {{"cache_key", response.cacheKey},
            {"generation_id", response.generationId},
            {"cached", response.cached},
            {"audio_url", response.audioUrl},
            {"audio_format", response.audioFormat},
            {"sample_rate", response.sampleRate},
            {"duration_seconds", response.durationSeconds},
            {"voice_segments", std::move(segments)},
            {"forced_alignment_words", timingsToJson(response.forcedAlignmentWords)},
            {"forced_alignment_chars", timingsToJson(response.forcedAlignmentChars)},
            {"forced_alignment_loss", response.forcedAlignmentLoss}};
}

inline nlohmann::json dialogPreviewLookupResponseToJson(const DialogPreviewLookupResponse &response) {
    auto generations = nlohmann::json::array();
    for (const auto &generation : response.generations)
        generations.push_back({{"generation_id", generation.generationId}, {"created_at", generation.createdAt}});
    return {{"cache_key", response.cacheKey},
            {"generations", std::move(generations)},
            {"latest_generation_id", response.latestGenerationId}};
}

inline nlohmann::json dialogScriptValidationResponseToJson(const DialogScriptValidationResponse &response) {
    nlohmann::json json{{"valid", response.valid},
                        {"turn_count", response.turnCount},
                        {"missing_creature_ids", response.missingCreatureIds},
                        {"error_messages", response.errorMessages}};
    if (response.scriptId)
        json["script_id"] = *response.scriptId;
    return json;
}

} // namespace creatures::api
