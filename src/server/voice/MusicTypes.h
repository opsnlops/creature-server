#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

/// Neutral model of an ElevenLabs Music generation request (#200).
///
/// The field names and limits mirror `POST /v1/music/detailed` in the live
/// OpenAPI spec so one struct can be validated at our API boundary
/// (`api::dialogMusicRequestFromJson`), stored as job details, and serialized
/// straight into the upstream body. Limits live here so the contract parser and
/// the client can never disagree.
namespace creatures::voice {

inline constexpr int64_t kMinMusicLengthMs = 3000;
inline constexpr int64_t kMaxMusicLengthMs = 600000;
inline constexpr int64_t kMaxMusicDurationExtensionMs = 60000;
inline constexpr std::size_t kMaxMusicPromptBytes = 4100;

// Composition-plan limits (GenerationChunk / AudioRefChunk / CompositionPlan).
inline constexpr std::size_t kMaxMusicPlanChunks = 30;
inline constexpr int64_t kMinMusicChunkDurationMs = 3000;
inline constexpr int64_t kMaxMusicChunkDurationMs = 120000;
inline constexpr std::size_t kMaxMusicChunkTextBytes = 6132;
inline constexpr std::size_t kMaxMusicStyles = 50;
inline constexpr std::size_t kMaxMusicStyleBytes = 200;
inline constexpr std::size_t kMaxMusicSongIdBytes = 100;
/// Cap on the whole plan's canonical JSON. The per-field limits above allow a
/// ~800 KB plan, and provenance stores the plan three times (upstream
/// request, upstream response metadata, plan) inside a WAV iXML chunk whose
/// reader stops at 1 MiB and a cache sidecar capped at 2 MB — so a maximal
/// plan could be generated but never loaded or promoted. 128 KB × 3, plus
/// XML-escaping growth, stays comfortably under both caps and is still ~10×
/// any plan the ElevenLabs planner produces.
inline constexpr std::size_t kMaxMusicPlanJsonBytes = 128 * 1024;
inline constexpr int64_t kMaxMusicSeed = 2147483647;
inline constexpr std::size_t kMaxMusicFinetuneIdBytes = 100;
inline constexpr double kMinMusicFinetuneStrength = 0.0;
inline constexpr double kMaxMusicFinetuneStrength = 2.0;

inline constexpr const char *kDefaultMusicModelId = "music_v2_5";

inline bool isSupportedMusicModelId(const std::string &modelId) {
    return modelId == "music_v2" || modelId == "music_v2_5";
}

inline bool isSupportedMusicGenerationMode(const std::string &mode) {
    return mode == "track" || mode == "loop" || mode == "ambience";
}

inline bool isSupportedContextAdherence(const std::string &value) {
    return value == "low" || value == "medium" || value == "high";
}

inline bool isSupportedConditionStrength(const std::string &value) {
    return value == "low" || value == "medium" || value == "high" || value == "xhigh";
}

/// A span of a previously generated song, by ElevenLabs `song_id`.
struct MusicAudioRange {
    std::string songId;
    int64_t startMs{0};
    int64_t endMs{0};

    bool operator==(const MusicAudioRange &) const = default;
};

/// One chunk of a composition plan. `audioRef` set → AudioRefChunk (re-render
/// that span of a prior take; close, not sample-exact); otherwise a
/// GenerationChunk.
struct MusicPlanChunk {
    std::optional<MusicAudioRange> audioRef;

    std::string text;
    int64_t durationMs{0};
    std::vector<std::string> positiveStyles;
    std::vector<std::string> negativeStyles;
    std::string contextAdherence = "high";
    std::optional<MusicAudioRange> conditioningRef;
    std::optional<std::string> conditionStrength;

    [[nodiscard]] bool isAudioRef() const { return audioRef.has_value(); }
    [[nodiscard]] int64_t effectiveDurationMs() const {
        return audioRef ? audioRef->endMs - audioRef->startMs : durationMs;
    }

    bool operator==(const MusicPlanChunk &) const = default;
};

struct MusicCompositionPlan {
    std::vector<MusicPlanChunk> chunks;

    [[nodiscard]] int64_t totalDurationMs() const {
        int64_t total = 0;
        for (const auto &chunk : chunks) {
            total += chunk.effectiveDurationMs();
        }
        return total;
    }

    bool operator==(const MusicCompositionPlan &) const = default;
};

/// Everything a `POST /v1/music/detailed` call can be told. Exactly one of
/// `prompt` / `compositionPlan` is populated; the prompt-only and plan-only
/// fields are documented in docs/200-music-controls-plan.md.
struct MusicGenerationRequest {
    std::string modelId = kDefaultMusicModelId;
    std::optional<std::string> finetuneId;
    std::optional<double> finetuneStrength;
    bool storeForInpainting = true;

    // Prompt mode
    std::string prompt;
    int64_t musicLengthMs{0};
    std::string generationMode = "track";
    bool forceInstrumental = true;

    // Plan mode
    std::optional<MusicCompositionPlan> compositionPlan;
    std::optional<int64_t> seed;

    [[nodiscard]] bool isPlanMode() const { return compositionPlan.has_value(); }
    [[nodiscard]] const char *requestKind() const { return isPlanMode() ? "composition_plan" : "prompt"; }
};

inline nlohmann::json musicAudioRangeToJson(const MusicAudioRange &range) {
    return {{"song_id", range.songId}, {"range", {{"start_ms", range.startMs}, {"end_ms", range.endMs}}}};
}

inline nlohmann::json musicPlanChunkToJson(const MusicPlanChunk &chunk) {
    if (chunk.audioRef) {
        return musicAudioRangeToJson(*chunk.audioRef);
    }
    nlohmann::json json{{"text", chunk.text},
                        {"duration_ms", chunk.durationMs},
                        {"positive_styles", chunk.positiveStyles},
                        {"negative_styles", chunk.negativeStyles},
                        {"context_adherence", chunk.contextAdherence}};
    if (chunk.conditioningRef) {
        json["conditioning_ref"] = musicAudioRangeToJson(*chunk.conditioningRef);
    }
    if (chunk.conditionStrength) {
        json["condition_strength"] = *chunk.conditionStrength;
    }
    return json;
}

inline nlohmann::json musicCompositionPlanToJson(const MusicCompositionPlan &plan) {
    auto chunks = nlohmann::json::array();
    for (const auto &chunk : plan.chunks) {
        chunks.push_back(musicPlanChunkToJson(chunk));
    }
    return {{"chunks", std::move(chunks)}};
}

/// The exact upstream body. Fields ElevenLabs forbids in the other mode are
/// omitted rather than sent as null, because the API rejects the combination
/// (`seed` with `prompt`, `force_instrumental` with `composition_plan`).
inline nlohmann::json musicGenerationRequestToElevenLabsJson(const MusicGenerationRequest &request) {
    nlohmann::json json{{"model_id", request.modelId},
                        {"store_for_inpainting", request.storeForInpainting},
                        {"with_timestamps", false},
                        {"sign_with_c2pa", false}};
    if (request.finetuneId) {
        json["finetune_id"] = *request.finetuneId;
        json["finetune_strength"] = request.finetuneStrength.value_or(1.0);
    }
    if (request.compositionPlan) {
        json["composition_plan"] = musicCompositionPlanToJson(*request.compositionPlan);
        if (request.seed) {
            json["seed"] = *request.seed;
        }
    } else {
        json["prompt"] = request.prompt;
        json["music_length_ms"] = request.musicLengthMs;
        json["generation_mode"] = request.generationMode;
        json["force_instrumental"] = request.forceInstrumental;
    }
    return json;
}

} // namespace creatures::voice
