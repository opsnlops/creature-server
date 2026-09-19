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

/// Dialog background-music generation (#200). Prompt mode is the original
/// contract; composition-plan mode is the console's fine-control path. Exactly
/// one of `prompt` / `compositionPlan` is populated — see
/// docs/200-music-controls-plan.md for which fields belong to which mode.
struct DialogMusicRequest {
    std::string scriptId;
    std::string dialogCacheKey;
    std::string dialogGenerationId;
    std::string prompt;
    int64_t durationExtensionMs = 0;
    std::string generationMode = "track";

    std::string modelId = voice::kDefaultMusicModelId;
    bool forceInstrumental = true;
    std::optional<std::string> finetuneId;
    std::optional<double> finetuneStrength;
    bool storeForInpainting = true;
    std::optional<voice::MusicCompositionPlan> compositionPlan;
    std::optional<int64_t> seed;

    [[nodiscard]] bool isPlanMode() const { return compositionPlan.has_value(); }
};

/// `POST /api/v1/animation/dialog/music/plan`: size a plan from a cached take.
struct DialogMusicPlanRequest {
    std::string dialogCacheKey;
    std::string dialogGenerationId;
    std::string prompt;
    int64_t durationExtensionMs = 0;
    std::string modelId = voice::kDefaultMusicModelId;
    std::optional<voice::MusicCompositionPlan> sourceCompositionPlan;
};

struct DialogMusicPlanResult {
    std::string modelId;
    int64_t musicLengthMs = 0;
    int64_t dialogDurationMs = 0;
    int64_t durationExtensionMs = 0;
    nlohmann::json compositionPlan;
};

struct AcceptVoiceTakeRequest {
    std::string scriptId;
    std::string generationId;
    std::string dialogCacheKey;
};

/// The knobs a take was made with, echoed back so the console can feed a take
/// into the next request (audio reference, conditioning, edit-a-chunk).
struct DialogMusicRecipe {
    std::string modelId;
    std::string songId;
    std::string requestKind; // "prompt" | "composition_plan"
    std::string prompt;
    std::optional<std::string> generationMode;
    std::optional<bool> forceInstrumental;
    std::optional<int64_t> seed;
    std::optional<std::string> finetuneId;
    std::optional<double> finetuneStrength;
    bool storedForInpainting = false;
    nlohmann::json compositionPlan; // the plan ElevenLabs actually used
    nlohmann::json songMetadata;
};

struct DialogMusicGenerationResult {
    std::string musicGenerationId;
    std::string mp3Url;
    double durationSeconds = 0;
    int64_t dialogDurationMs = 0;
    int64_t durationExtensionMs = 0;
    int64_t requestedMusicLengthMs = 0;
    std::string prompt;
    DialogMusicRecipe recipe;
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

namespace detail {

inline Result<std::vector<std::string>> musicStyleListFromJson(const nlohmann::json &json, const std::string &path,
                                                               std::string_view key, bool required) {
    using ListResult = Result<std::vector<std::string>>;
    if (!required && !json.contains(key)) {
        return ListResult{std::vector<std::string>{}};
    }
    auto array = json_codec::requiredArray(json, path, key, voice::kMaxMusicStyles);
    if (!array.isSuccess())
        return ListResult{array.getError().value()};
    std::vector<std::string> styles;
    std::size_t index = 0;
    for (const auto &item : array.getValue().value().get()) {
        const auto itemPath = fmt::format("{}.{}[{}]", path, key, index++);
        if (!item.is_string())
            return json_codec::invalid<std::vector<std::string>>(fmt::format("{} must be a string", itemPath));
        auto value = item.get<std::string>();
        if (value.empty() || value.size() > voice::kMaxMusicStyleBytes)
            return json_codec::invalid<std::vector<std::string>>(
                fmt::format("{} must be 1-{} bytes", itemPath, voice::kMaxMusicStyleBytes));
        styles.push_back(std::move(value));
    }
    return ListResult{std::move(styles)};
}

inline Result<voice::MusicAudioRange> musicAudioRangeFromJson(const nlohmann::json &json, const std::string &path) {
    using RangeResult = Result<voice::MusicAudioRange>;
    auto fields = json_codec::rejectUnknownFields(json, path, {"song_id", "range"});
    if (!fields.isSuccess())
        return RangeResult{fields.getError().value()};
    auto songId = json_codec::requiredString(json, path, "song_id", voice::kMaxMusicSongIdBytes);
    if (!songId.isSuccess())
        return RangeResult{songId.getError().value()};
    const auto rangeIterator = json.find("range");
    const auto rangePath = path + ".range";
    if (rangeIterator == json.end())
        return json_codec::invalid<voice::MusicAudioRange>(rangePath + " is required");
    auto rangeFields = json_codec::rejectUnknownFields(*rangeIterator, rangePath, {"start_ms", "end_ms"});
    if (!rangeFields.isSuccess())
        return RangeResult{rangeFields.getError().value()};
    auto start = json_codec::requiredInt64(*rangeIterator, rangePath, "start_ms", 0, voice::kMaxMusicLengthMs);
    auto end = json_codec::requiredInt64(*rangeIterator, rangePath, "end_ms", 0, voice::kMaxMusicLengthMs);
    if (!start.isSuccess())
        return RangeResult{start.getError().value()};
    if (!end.isSuccess())
        return RangeResult{end.getError().value()};
    voice::MusicAudioRange range{songId.getValue().value(), start.getValue().value(), end.getValue().value()};
    if (range.endMs <= range.startMs)
        return json_codec::invalid<voice::MusicAudioRange>(rangePath + ".end_ms must be greater than start_ms");
    return RangeResult{std::move(range)};
}

inline Result<voice::MusicPlanChunk> musicPlanChunkFromJson(const nlohmann::json &json, const std::string &path) {
    using ChunkResult = Result<voice::MusicPlanChunk>;
    auto object = json_codec::requireObject(json, path);
    if (!object.isSuccess())
        return ChunkResult{object.getError().value()};
    voice::MusicPlanChunk chunk;
    const bool hasSongId = json.contains("song_id");
    const bool hasText = json.contains("text");
    if (hasSongId == hasText)
        return json_codec::invalid<voice::MusicPlanChunk>(
            path + " must be either an audio reference (song_id + range) or a generation chunk (text + duration_ms)");
    if (hasSongId) {
        auto range = musicAudioRangeFromJson(json, path);
        if (!range.isSuccess())
            return ChunkResult{range.getError().value()};
        const auto lengthMs = range.getValue().value().endMs - range.getValue().value().startMs;
        if (lengthMs < voice::kMinMusicChunkDurationMs || lengthMs > voice::kMaxMusicChunkDurationMs)
            return json_codec::invalid<voice::MusicPlanChunk>(fmt::format(
                "{}.range must span {}-{} ms", path, voice::kMinMusicChunkDurationMs, voice::kMaxMusicChunkDurationMs));
        chunk.audioRef = range.getValue().value();
        return ChunkResult{std::move(chunk)};
    }
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"text", "duration_ms", "positive_styles", "negative_styles",
                                                   "context_adherence", "conditioning_ref", "condition_strength"});
    if (!fields.isSuccess())
        return ChunkResult{fields.getError().value()};
    auto text = json_codec::requiredString(json, path, "text", voice::kMaxMusicChunkTextBytes, true);
    auto duration = json_codec::requiredInt64(json, path, "duration_ms", voice::kMinMusicChunkDurationMs,
                                              voice::kMaxMusicChunkDurationMs);
    auto positive = musicStyleListFromJson(json, path, "positive_styles", true);
    auto negative = musicStyleListFromJson(json, path, "negative_styles", false);
    auto adherence = json_codec::optionalString(json, path, "context_adherence", 16);
    // ElevenLabs' own plan output carries explicit nulls for the two
    // conditioning fields, and the console sends a drafted plan back as-is,
    // so null is documented as "absent" here (json-codec-conventions.md).
    auto strength = json_codec::optionalString(json, path, "condition_strength", 16, false, true);
    if (!text.isSuccess())
        return ChunkResult{text.getError().value()};
    if (!duration.isSuccess())
        return ChunkResult{duration.getError().value()};
    if (!positive.isSuccess())
        return ChunkResult{positive.getError().value()};
    if (!negative.isSuccess())
        return ChunkResult{negative.getError().value()};
    if (!adherence.isSuccess())
        return ChunkResult{adherence.getError().value()};
    if (!strength.isSuccess())
        return ChunkResult{strength.getError().value()};
    chunk.text = text.getValue().value();
    chunk.durationMs = duration.getValue().value();
    chunk.positiveStyles = positive.getValue().value();
    chunk.negativeStyles = negative.getValue().value();
    chunk.contextAdherence = adherence.getValue().value().value_or("high");
    if (!voice::isSupportedContextAdherence(chunk.contextAdherence))
        return json_codec::invalid<voice::MusicPlanChunk>(path +
                                                          ".context_adherence must be 'low', 'medium', or 'high'");
    if (json.contains("conditioning_ref") && !json["conditioning_ref"].is_null()) {
        auto ref = musicAudioRangeFromJson(json["conditioning_ref"], path + ".conditioning_ref");
        if (!ref.isSuccess())
            return ChunkResult{ref.getError().value()};
        chunk.conditioningRef = ref.getValue().value();
    }
    chunk.conditionStrength = strength.getValue().value();
    if (chunk.conditionStrength) {
        if (!voice::isSupportedConditionStrength(*chunk.conditionStrength))
            return json_codec::invalid<voice::MusicPlanChunk>(
                path + ".condition_strength must be 'low', 'medium', 'high', or 'xhigh'");
        if (!chunk.conditioningRef)
            return json_codec::invalid<voice::MusicPlanChunk>(path + ".condition_strength requires conditioning_ref");
    }
    return ChunkResult{std::move(chunk)};
}

inline Result<voice::MusicCompositionPlan> musicCompositionPlanFromJson(const nlohmann::json &json,
                                                                        const std::string &path) {
    using PlanResult = Result<voice::MusicCompositionPlan>;
    auto fields = json_codec::rejectUnknownFields(json, path, {"chunks"});
    if (!fields.isSuccess())
        return PlanResult{fields.getError().value()};
    auto chunks = json_codec::requiredArray(json, path, "chunks", voice::kMaxMusicPlanChunks, 1);
    if (!chunks.isSuccess())
        return PlanResult{chunks.getError().value()};
    voice::MusicCompositionPlan plan;
    std::size_t index = 0;
    for (const auto &item : chunks.getValue().value().get()) {
        auto chunk = musicPlanChunkFromJson(item, fmt::format("{}.chunks[{}]", path, index++));
        if (!chunk.isSuccess())
            return PlanResult{chunk.getError().value()};
        plan.chunks.push_back(chunk.getValue().value());
    }
    const auto total = plan.totalDurationMs();
    if (total < voice::kMinMusicLengthMs || total > voice::kMaxMusicLengthMs)
        return json_codec::invalid<voice::MusicCompositionPlan>(fmt::format("{}.chunks total {} ms; must be {}-{} ms",
                                                                            path, total, voice::kMinMusicLengthMs,
                                                                            voice::kMaxMusicLengthMs));
    // Whole-plan size: see kMaxMusicPlanJsonBytes for why the per-field
    // limits alone aren't enough (the plan is embedded in the take's iXML).
    if (const auto bytes = voice::musicCompositionPlanToJson(plan).dump().size(); bytes > voice::kMaxMusicPlanJsonBytes)
        return json_codec::invalid<voice::MusicCompositionPlan>(
            fmt::format("{} serializes to {} bytes; maximum is {}", path, bytes, voice::kMaxMusicPlanJsonBytes));
    return PlanResult{std::move(plan)};
}

} // namespace detail

inline Result<DialogMusicRequest> dialogMusicRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "dialog music request";
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"script_id", "dialog_cache_key", "dialog_generation_id", "prompt",
                                                   "duration_extension_ms", "generation_mode", "model_id",
                                                   "force_instrumental", "finetune_id", "finetune_strength",
                                                   "store_for_inpainting", "composition_plan", "seed"});
    if (!fields.isSuccess())
        return Result<DialogMusicRequest>{fields.getError().value()};
    auto scriptId = json_codec::requiredString(json, path, "script_id", 64);
    auto cacheKey = json_codec::requiredString(json, path, "dialog_cache_key", 64);
    auto generationId = json_codec::requiredString(json, path, "dialog_generation_id", 64);
    auto prompt = json_codec::optionalString(json, path, "prompt", voice::kMaxMusicPromptBytes);
    auto duration =
        json_codec::optionalInt64(json, path, "duration_extension_ms", 0, voice::kMaxMusicDurationExtensionMs, true);
    auto mode = json_codec::optionalString(json, path, "generation_mode", 16, false, true);
    auto modelId = json_codec::optionalString(json, path, "model_id", 32, false, true);
    auto forceInstrumental = json_codec::optionalBool(json, path, "force_instrumental", true);
    auto finetuneId =
        json_codec::optionalString(json, path, "finetune_id", voice::kMaxMusicFinetuneIdBytes, false, true);
    auto finetuneStrength = json_codec::optionalFiniteDouble(
        json, path, "finetune_strength", voice::kMinMusicFinetuneStrength, voice::kMaxMusicFinetuneStrength, true);
    auto storeForInpainting = json_codec::optionalBool(json, path, "store_for_inpainting", true);
    auto seed = json_codec::optionalInt64(json, path, "seed", 0, voice::kMaxMusicSeed, true);
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
    if (!modelId.isSuccess())
        return Result<DialogMusicRequest>{modelId.getError().value()};
    if (!forceInstrumental.isSuccess())
        return Result<DialogMusicRequest>{forceInstrumental.getError().value()};
    if (!finetuneId.isSuccess())
        return Result<DialogMusicRequest>{finetuneId.getError().value()};
    if (!finetuneStrength.isSuccess())
        return Result<DialogMusicRequest>{finetuneStrength.getError().value()};
    if (!storeForInpainting.isSuccess())
        return Result<DialogMusicRequest>{storeForInpainting.getError().value()};
    if (!seed.isSuccess())
        return Result<DialogMusicRequest>{seed.getError().value()};
    DialogMusicRequest request;
    request.scriptId = scriptId.getValue().value();
    request.dialogCacheKey = cacheKey.getValue().value();
    request.dialogGenerationId = generationId.getValue().value();
    request.prompt = prompt.getValue().value().value_or("");
    request.durationExtensionMs = duration.getValue().value().value_or(0);
    request.generationMode = mode.getValue().value().value_or("track");
    request.modelId = modelId.getValue().value().value_or(voice::kDefaultMusicModelId);
    request.forceInstrumental = forceInstrumental.getValue().value().value_or(true);
    request.finetuneId = finetuneId.getValue().value();
    request.finetuneStrength = finetuneStrength.getValue().value();
    request.storeForInpainting = storeForInpainting.getValue().value().value_or(true);
    request.seed = seed.getValue().value();
    if (json.contains("composition_plan")) {
        auto plan =
            detail::musicCompositionPlanFromJson(json["composition_plan"], std::string(path) + ".composition_plan");
        if (!plan.isSuccess())
            return Result<DialogMusicRequest>{plan.getError().value()};
        request.compositionPlan = plan.getValue().value();
    }
    if (!isUuidShape(request.scriptId))
        return json_codec::invalid<DialogMusicRequest>("dialog music request.script_id must be a UUID");
    if (!isLowercaseSha256(request.dialogCacheKey))
        return json_codec::invalid<DialogMusicRequest>(
            "dialog music request.dialog_cache_key must be a 64-character lowercase hex sha256");
    if (!isUuidShape(request.dialogGenerationId))
        return json_codec::invalid<DialogMusicRequest>("dialog music request.dialog_generation_id must be a UUID");
    if (!voice::isSupportedMusicModelId(request.modelId))
        return json_codec::invalid<DialogMusicRequest>(
            "dialog music request.model_id must be 'music_v2' or 'music_v2_5'");
    if (request.finetuneStrength && !request.finetuneId)
        return json_codec::invalid<DialogMusicRequest>("dialog music request.finetune_strength requires finetune_id");
    // Exactly one authoring mode. ElevenLabs rejects the cross-mode fields
    // outright, and its 4xx body never reaches the console (see
    // docs/200-music-controls-plan.md), so say precisely which field is wrong.
    if (request.isPlanMode()) {
        if (!request.prompt.empty())
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.prompt cannot be combined with composition_plan");
        if (json.contains("duration_extension_ms"))
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.duration_extension_ms only applies to prompt mode; the plan sets the length");
        if (json.contains("generation_mode"))
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.generation_mode only applies to prompt mode");
        if (json.contains("force_instrumental"))
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.force_instrumental only applies to prompt mode; leave a chunk's text free "
                "of lyrics for an instrumental section");
    } else {
        if (request.prompt.empty())
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request requires either prompt or composition_plan");
        if (request.seed)
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.seed only applies to composition_plan mode");
        if (!voice::isSupportedMusicGenerationMode(request.generationMode))
            return json_codec::invalid<DialogMusicRequest>(
                "dialog music request.generation_mode must be 'track', 'loop', or 'ambience'");
    }
    return Result<DialogMusicRequest>{std::move(request)};
}

inline Result<DialogMusicPlanRequest> dialogMusicPlanRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "dialog music plan request";
    using PlanRequestResult = Result<DialogMusicPlanRequest>;
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"dialog_cache_key", "dialog_generation_id", "prompt",
                                                   "duration_extension_ms", "model_id", "source_composition_plan"});
    if (!fields.isSuccess())
        return PlanRequestResult{fields.getError().value()};
    auto cacheKey = json_codec::requiredString(json, path, "dialog_cache_key", 64);
    auto generationId = json_codec::requiredString(json, path, "dialog_generation_id", 64);
    auto prompt = json_codec::requiredString(json, path, "prompt", voice::kMaxMusicPromptBytes);
    auto duration =
        json_codec::optionalInt64(json, path, "duration_extension_ms", 0, voice::kMaxMusicDurationExtensionMs, true);
    auto modelId = json_codec::optionalString(json, path, "model_id", 32, false, true);
    if (!cacheKey.isSuccess())
        return PlanRequestResult{cacheKey.getError().value()};
    if (!generationId.isSuccess())
        return PlanRequestResult{generationId.getError().value()};
    if (!prompt.isSuccess())
        return PlanRequestResult{prompt.getError().value()};
    if (!duration.isSuccess())
        return PlanRequestResult{duration.getError().value()};
    if (!modelId.isSuccess())
        return PlanRequestResult{modelId.getError().value()};
    DialogMusicPlanRequest request;
    request.dialogCacheKey = cacheKey.getValue().value();
    request.dialogGenerationId = generationId.getValue().value();
    request.prompt = prompt.getValue().value();
    request.durationExtensionMs = duration.getValue().value().value_or(0);
    request.modelId = modelId.getValue().value().value_or(voice::kDefaultMusicModelId);
    if (json.contains("source_composition_plan")) {
        auto plan = detail::musicCompositionPlanFromJson(json["source_composition_plan"],
                                                         std::string(path) + ".source_composition_plan");
        if (!plan.isSuccess())
            return PlanRequestResult{plan.getError().value()};
        request.sourceCompositionPlan = plan.getValue().value();
    }
    if (!isLowercaseSha256(request.dialogCacheKey))
        return json_codec::invalid<DialogMusicPlanRequest>(
            "dialog music plan request.dialog_cache_key must be a 64-character lowercase hex sha256");
    if (!isUuidShape(request.dialogGenerationId))
        return json_codec::invalid<DialogMusicPlanRequest>(
            "dialog music plan request.dialog_generation_id must be a UUID");
    if (!voice::isSupportedMusicModelId(request.modelId))
        return json_codec::invalid<DialogMusicPlanRequest>(
            "dialog music plan request.model_id must be 'music_v2' or 'music_v2_5'");
    return PlanRequestResult{std::move(request)};
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

/// Must round-trip through dialogMusicRequestFromJson exactly: this is the
/// job `details` payload the worker re-parses. Mode-specific fields are
/// omitted in the other mode because the parser rejects them there.
inline nlohmann::json dialogMusicRequestToJson(const DialogMusicRequest &request) {
    nlohmann::json json{{"script_id", request.scriptId},
                        {"dialog_cache_key", request.dialogCacheKey},
                        {"dialog_generation_id", request.dialogGenerationId},
                        {"model_id", request.modelId},
                        {"store_for_inpainting", request.storeForInpainting}};
    if (request.finetuneId) {
        json["finetune_id"] = *request.finetuneId;
        if (request.finetuneStrength)
            json["finetune_strength"] = *request.finetuneStrength;
    }
    if (request.compositionPlan) {
        json["composition_plan"] = voice::musicCompositionPlanToJson(*request.compositionPlan);
        if (request.seed)
            json["seed"] = *request.seed;
    } else {
        json["prompt"] = request.prompt;
        json["duration_extension_ms"] = request.durationExtensionMs;
        json["generation_mode"] = request.generationMode;
        json["force_instrumental"] = request.forceInstrumental;
    }
    return json;
}

inline nlohmann::json dialogMusicRecipeToJson(const DialogMusicRecipe &recipe) {
    nlohmann::json json{
        {"model_id", recipe.modelId},
        {"song_id", recipe.songId},
        {"request_kind", recipe.requestKind},
        {"stored_for_inpainting", recipe.storedForInpainting},
        {"composition_plan", recipe.compositionPlan.is_null() ? nlohmann::json::object() : recipe.compositionPlan},
        {"song_metadata", recipe.songMetadata.is_null() ? nlohmann::json::object() : recipe.songMetadata}};
    if (!recipe.prompt.empty())
        json["prompt"] = recipe.prompt;
    if (recipe.generationMode)
        json["generation_mode"] = *recipe.generationMode;
    if (recipe.forceInstrumental)
        json["force_instrumental"] = *recipe.forceInstrumental;
    if (recipe.seed)
        json["seed"] = *recipe.seed;
    if (recipe.finetuneId)
        json["finetune_id"] = *recipe.finetuneId;
    if (recipe.finetuneStrength)
        json["finetune_strength"] = *recipe.finetuneStrength;
    return json;
}

/// The original seven keys are unchanged for pre-#200 consoles; the recipe
/// keys are added beside them.
inline nlohmann::json dialogMusicGenerationResultToJson(const DialogMusicGenerationResult &result) {
    nlohmann::json json{{"music_generation_id", result.musicGenerationId},
                        {"mp3_url", result.mp3Url},
                        {"duration_seconds", result.durationSeconds},
                        {"dialog_duration_ms", result.dialogDurationMs},
                        {"duration_extension_ms", result.durationExtensionMs},
                        {"requested_music_length_ms", result.requestedMusicLengthMs},
                        {"prompt", result.prompt}};
    // The recipe only carries "prompt" when non-empty, so the legacy key set
    // above survives update() untouched in plan mode.
    json.update(dialogMusicRecipeToJson(result.recipe));
    return json;
}

inline nlohmann::json dialogMusicPlanResultToJson(const DialogMusicPlanResult &result) {
    return {{"model_id", result.modelId},
            {"music_length_ms", result.musicLengthMs},
            {"dialog_duration_ms", result.dialogDurationMs},
            {"duration_extension_ms", result.durationExtensionMs},
            {"composition_plan", result.compositionPlan}};
}

inline nlohmann::json musicFinetuneToJson(const voice::MusicFinetune &finetune) {
    nlohmann::json json{{"finetune_id", finetune.id},
                        {"name", finetune.name},
                        {"model_id", finetune.modelId},
                        {"status", finetune.status},
                        {"visibility", finetune.visibility},
                        {"created_by", finetune.createdBy},
                        {"tags", finetune.tags},
                        {"training_progress", finetune.trainingProgress}};
    if (finetune.primaryGenre)
        json["primary_genre"] = *finetune.primaryGenre;
    return json;
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
    nlohmann::json json{{"cache_key", response.cacheKey}, {"generations", std::move(generations)}};
    // Omitted, not empty, when nothing is cached (#204) — per the codec
    // conventions, and the console decodes it as optional.
    if (!response.latestGenerationId.empty())
        json["latest_generation_id"] = response.latestGenerationId;
    return json;
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
