#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "api/DialogContracts.h"
#include "api/JsonResponse.h"
#include "model/JsonCodec.h"
#include "model/MusicPiece.h"
#include "server/voice/MusicTypes.h"
#include "util/Result.h"
#include "util/helpers.h"

/// Music library contracts (#202): dialog-free generation, saving candidates
/// as pieces, and editing pieces. See docs/music-library-plan.md.
namespace creatures::api {

/// `POST /api/v1/music/generate`. The dialog request's knobs without the
/// dialog binding, plus a third authoring mode — `sections` — which is the
/// refinement builder's input. Exactly one of prompt / composition_plan /
/// sections. Serialised as job details and re-parsed by the worker, so
/// musicGenerateRequestToJson must round-trip exactly.
struct MusicGenerateRequest : DialogMusicRequest {
    int64_t musicLengthMs = 0; // prompt mode only
    std::optional<std::vector<voice::MusicSection>> sections;
    std::string baseVersionId;
    std::vector<std::size_t> keep;
    std::string conditionStrength = "medium";
    std::string pieceId;

    [[nodiscard]] bool isSectionsMode() const { return sections.has_value(); }
    [[nodiscard]] const char *requestKind() const {
        return isSectionsMode() ? "sections" : isPlanMode() ? "composition_plan" : "prompt";
    }
};

struct MusicSaveRequest {
    std::string title;
    std::string notes;
    std::string pieceId;
    std::optional<std::vector<voice::MusicSection>> sections;
};

struct MusicPieceUpdateRequest {
    std::optional<std::string> title;
    std::optional<std::string> notes;
    std::optional<std::string> currentVersionId;
};

inline constexpr std::size_t MAX_MUSIC_REQUEST_BYTES = MAX_DIALOG_REQUEST_BYTES;

inline Result<MusicGenerateRequest> musicGenerateRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "music generate request";
    using RequestResult = Result<MusicGenerateRequest>;
    auto fields = json_codec::rejectUnknownFields(json, path,
                                                  {"prompt", "music_length_ms", "generation_mode", "force_instrumental",
                                                   "model_id", "finetune_id", "finetune_strength",
                                                   "store_for_inpainting", "composition_plan", "seed", "sections",
                                                   "base_version_id", "keep", "condition_strength", "piece_id"});
    if (!fields.isSuccess())
        return RequestResult{fields.getError().value()};
    MusicGenerateRequest request;
    auto length = json_codec::optionalInt64(json, path, "music_length_ms", voice::kMinMusicLengthMs,
                                            voice::kMaxMusicLengthMs, true);
    auto baseVersion = json_codec::optionalString(json, path, "base_version_id", 64, false, true);
    auto strength = json_codec::optionalString(json, path, "condition_strength", 16, false, true);
    auto pieceId = json_codec::optionalString(json, path, "piece_id", 64, false, true);
    if (!length.isSuccess())
        return RequestResult{length.getError().value()};
    if (!baseVersion.isSuccess())
        return RequestResult{baseVersion.getError().value()};
    if (!strength.isSuccess())
        return RequestResult{strength.getError().value()};
    if (!pieceId.isSuccess())
        return RequestResult{pieceId.getError().value()};
    request.musicLengthMs = length.getValue().value().value_or(0);
    request.baseVersionId = baseVersion.getValue().value().value_or("");
    request.conditionStrength = strength.getValue().value().value_or("medium");
    request.pieceId = pieceId.getValue().value().value_or("");
    if (!request.pieceId.empty() && !isUuidShape(request.pieceId))
        return json_codec::invalid<MusicGenerateRequest>("music generate request.piece_id must be a UUID");
    if (!request.baseVersionId.empty() && !isUuidShape(request.baseVersionId))
        return json_codec::invalid<MusicGenerateRequest>("music generate request.base_version_id must be a UUID");
    if (!voice::isSupportedConditionStrength(request.conditionStrength))
        return json_codec::invalid<MusicGenerateRequest>(
            "music generate request.condition_strength must be 'low', 'medium', 'high', or 'xhigh'");
    if (json.contains("sections")) {
        auto sections = musicSectionsFromJson(json["sections"], std::string(path) + ".sections");
        if (!sections.isSuccess())
            return RequestResult{sections.getError().value()};
        request.sections = sections.getValue().value();
    }
    if (json.contains("keep")) {
        auto keep = json_codec::requiredArray(json, path, "keep", voice::kMaxMusicPlanChunks);
        if (!keep.isSuccess())
            return RequestResult{keep.getError().value()};
        for (const auto &item : keep.getValue().value().get()) {
            if (!item.is_number_integer() || item.get<int64_t>() < 0 ||
                item.get<int64_t>() >= static_cast<int64_t>(voice::kMaxMusicPlanChunks))
                return json_codec::invalid<MusicGenerateRequest>(
                    fmt::format("music generate request.keep entries must be section indices 0-{}",
                                voice::kMaxMusicPlanChunks - 1));
            request.keep.push_back(item.get<std::size_t>());
        }
    }
    // Sections mode excludes the other two modes' fields; check that on the
    // raw JSON before the shared parser applies its own mode rules.
    if (request.isSectionsMode()) {
        if (json.contains("composition_plan"))
            return json_codec::invalid<MusicGenerateRequest>(
                "music generate request.sections cannot be combined with composition_plan");
        if (json.contains("prompt"))
            return json_codec::invalid<MusicGenerateRequest>(
                "music generate request.prompt cannot be combined with sections");
        for (const char *field : {"music_length_ms", "generation_mode", "force_instrumental"}) {
            if (json.contains(field))
                return json_codec::invalid<MusicGenerateRequest>(fmt::format(
                    "music generate request.{} only applies to prompt mode; the sections set the length", field));
        }
        for (const auto index : request.keep) {
            if (index >= request.sections->size())
                return json_codec::invalid<MusicGenerateRequest>(
                    fmt::format("music generate request.keep[{}] is beyond the last section", index));
        }
        if (!request.keep.empty() && request.baseVersionId.empty())
            return json_codec::invalid<MusicGenerateRequest>("music generate request.keep requires base_version_id");
        if (!request.baseVersionId.empty() && request.pieceId.empty())
            return json_codec::invalid<MusicGenerateRequest>(
                "music generate request.base_version_id requires piece_id");
    } else {
        for (const char *field : {"keep", "base_version_id", "condition_strength"}) {
            if (json.contains(field))
                return json_codec::invalid<MusicGenerateRequest>(
                    fmt::format("music generate request.{} only applies to sections mode", field));
        }
    }
    // The shared knobs. Prompt mode is required only when neither of the
    // other two modes is present; in sections mode the parser only sees the
    // common fields (model, finetune, store, seed).
    const bool promptRequired = !request.isSectionsMode() && !json.contains("composition_plan");
    if (auto knobs = detail::parseMusicKnobs(json, path, request, promptRequired); !knobs.isSuccess())
        return RequestResult{knobs.getError().value()};
    if (!request.isSectionsMode() && !request.isPlanMode() && request.musicLengthMs == 0)
        return json_codec::invalid<MusicGenerateRequest>(
            "music generate request.music_length_ms is required in prompt mode");
    return RequestResult{std::move(request)};
}

/// Job-details payload; must round-trip through musicGenerateRequestFromJson.
inline nlohmann::json musicGenerateRequestToJson(const MusicGenerateRequest &request) {
    nlohmann::json json{{"model_id", request.modelId}, {"store_for_inpainting", request.storeForInpainting}};
    if (request.finetuneId) {
        json["finetune_id"] = *request.finetuneId;
        if (request.finetuneStrength)
            json["finetune_strength"] = *request.finetuneStrength;
    }
    if (!request.pieceId.empty())
        json["piece_id"] = request.pieceId;
    if (request.isSectionsMode()) {
        json["sections"] = voice::musicSectionsToJson(*request.sections);
        if (!request.baseVersionId.empty())
            json["base_version_id"] = request.baseVersionId;
        if (!request.keep.empty())
            json["keep"] = request.keep;
        json["condition_strength"] = request.conditionStrength;
        if (request.seed)
            json["seed"] = *request.seed;
    } else if (request.compositionPlan) {
        json["composition_plan"] = voice::musicCompositionPlanToJson(*request.compositionPlan);
        if (request.seed)
            json["seed"] = *request.seed;
    } else {
        json["prompt"] = request.prompt;
        json["music_length_ms"] = request.musicLengthMs;
        json["generation_mode"] = request.generationMode;
        json["force_instrumental"] = request.forceInstrumental;
    }
    return json;
}

inline Result<MusicSaveRequest> musicSaveRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "music save request";
    using SaveResult = Result<MusicSaveRequest>;
    auto fields = json_codec::rejectUnknownFields(json, path, {"title", "notes", "piece_id", "sections"});
    if (!fields.isSuccess())
        return SaveResult{fields.getError().value()};
    auto title = json_codec::optionalString(json, path, "title", MAX_MUSIC_PIECE_TITLE_BYTES, false, true);
    auto notes = json_codec::optionalString(json, path, "notes", MAX_MUSIC_PIECE_NOTES_BYTES, true, true);
    auto pieceId = json_codec::optionalString(json, path, "piece_id", 64, false, true);
    if (!title.isSuccess())
        return SaveResult{title.getError().value()};
    if (!notes.isSuccess())
        return SaveResult{notes.getError().value()};
    if (!pieceId.isSuccess())
        return SaveResult{pieceId.getError().value()};
    MusicSaveRequest request;
    request.title = title.getValue().value().value_or("");
    request.notes = notes.getValue().value().value_or("");
    request.pieceId = pieceId.getValue().value().value_or("");
    if (!request.pieceId.empty() && !isUuidShape(request.pieceId))
        return json_codec::invalid<MusicSaveRequest>("music save request.piece_id must be a UUID");
    if (request.pieceId.empty() && request.title.empty())
        return json_codec::invalid<MusicSaveRequest>("music save request.title is required for a new piece");
    if (json.contains("sections")) {
        auto sections = musicSectionsFromJson(json["sections"], std::string(path) + ".sections");
        if (!sections.isSuccess())
            return SaveResult{sections.getError().value()};
        request.sections = sections.getValue().value();
    }
    return SaveResult{std::move(request)};
}

inline Result<MusicPieceUpdateRequest> musicPieceUpdateRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "music piece update request";
    using UpdateResult = Result<MusicPieceUpdateRequest>;
    auto fields = json_codec::rejectUnknownFields(json, path, {"title", "notes", "current_version_id"});
    if (!fields.isSuccess())
        return UpdateResult{fields.getError().value()};
    auto title = json_codec::optionalString(json, path, "title", MAX_MUSIC_PIECE_TITLE_BYTES);
    auto notes = json_codec::optionalString(json, path, "notes", MAX_MUSIC_PIECE_NOTES_BYTES, true);
    auto current = json_codec::optionalString(json, path, "current_version_id", 64);
    if (!title.isSuccess())
        return UpdateResult{title.getError().value()};
    if (!notes.isSuccess())
        return UpdateResult{notes.getError().value()};
    if (!current.isSuccess())
        return UpdateResult{current.getError().value()};
    MusicPieceUpdateRequest request{title.getValue().value(), notes.getValue().value(), current.getValue().value()};
    if (!request.title && !request.notes && !request.currentVersionId)
        return json_codec::invalid<MusicPieceUpdateRequest>("music piece update request has nothing to change");
    if (request.currentVersionId && !isUuidShape(*request.currentVersionId))
        return json_codec::invalid<MusicPieceUpdateRequest>(
            "music piece update request.current_version_id must be a UUID");
    return UpdateResult{std::move(request)};
}

} // namespace creatures::api
