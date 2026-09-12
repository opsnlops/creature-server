#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "api/StreamingAdHocContracts.h"
#include "model/JsonCodec.h"
#include "util/Result.h"
#include "util/UuidValidation.h"

// Wire contracts for the streamed multi-character dialog (issue #186):
// POST /api/v1/animation/dialog-stream/{start,turn,finish}. The shapes mirror
// the single-creature ad-hoc stream so a client that speaks one speaks both.
namespace creatures::api {

inline constexpr std::size_t MAX_DIALOG_STREAM_PARTICIPANTS = 8;
inline constexpr std::size_t MAX_DIALOG_STREAM_TEXT_BYTES = MAX_STREAMING_AD_HOC_TEXT_BYTES;

struct DialogStreamStartRequest {
    std::vector<std::string> creatureIds; // as submitted; 1..8, no duplicates
    std::string stageId;                  // required: the birds have to look at each other
    bool resumePlaylist{true};
};

struct DialogStreamTurnRequest {
    std::string sessionId;
    std::string creatureId;
    std::string text;
};

struct DialogStreamFinishRequest {
    std::string sessionId;
};

struct DialogStreamStartResponse {
    std::string sessionId;
    std::string status;
    std::string message;
    std::vector<std::string> creatureIds;
    std::string stageId;
};

struct DialogStreamTurnResponse {
    std::string sessionId;
    std::string status;
    int turnsReceived;
};

struct DialogStreamFinishResponse {
    std::string sessionId;
    std::string status;
    std::string message;
    std::string animationId; // the whole exchange, every participant's track; empty if the stitch failed
    std::string lastTurnAnimationId;
    bool playbackTriggered;
    std::string exchangeStatus;
    int partsRendered;
    int partsTotal;
};

inline Result<DialogStreamStartRequest> dialogStreamStartRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "dialog stream start request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"creature_ids", "stage_id", "resume_playlist"});
    if (!fields.isSuccess())
        return Result<DialogStreamStartRequest>{fields.getError().value()};

    auto creatureIdsResult = json_codec::requiredArray(json, path, "creature_ids", MAX_DIALOG_STREAM_PARTICIPANTS, 1);
    if (!creatureIdsResult.isSuccess())
        return Result<DialogStreamStartRequest>{creatureIdsResult.getError().value()};
    const auto &creatureIdsJson = creatureIdsResult.getValue()->get();

    DialogStreamStartRequest request;
    request.creatureIds.reserve(creatureIdsJson.size());
    std::unordered_set<std::string> seen;
    for (std::size_t index = 0; index < creatureIdsJson.size(); ++index) {
        const auto &entry = creatureIdsJson[index];
        if (!entry.is_string())
            return json_codec::invalid<DialogStreamStartRequest>(
                fmt::format("{}.creature_ids[{}] must be a string", path, index));
        const auto creatureId = entry.get<std::string>();
        if (!isUuidShape(creatureId))
            return json_codec::invalid<DialogStreamStartRequest>(
                fmt::format("{}.creature_ids[{}] must be a UUID", path, index));
        // Two spellings of one creature are still one creature.
        if (!seen.insert(canonicalUuid(creatureId)).second)
            return json_codec::invalid<DialogStreamStartRequest>(
                fmt::format("{}.creature_ids[{}] repeats an earlier creature", path, index));
        // Historical Mongo IDs are case-sensitive strings: preserve submitted
        // spelling for lookups, like streamingUuid does.
        request.creatureIds.push_back(creatureId);
    }

    auto stageId = streamingUuid(json, path, "stage_id");
    if (!stageId.isSuccess())
        return Result<DialogStreamStartRequest>{stageId.getError().value()};
    request.stageId = stageId.getValue().value();

    auto resumePlaylist = json_codec::optionalBool(json, path, "resume_playlist");
    if (!resumePlaylist.isSuccess())
        return Result<DialogStreamStartRequest>{resumePlaylist.getError().value()};
    request.resumePlaylist = resumePlaylist.getValue()->value_or(true);
    return Result<DialogStreamStartRequest>{std::move(request)};
}

inline Result<DialogStreamTurnRequest> dialogStreamTurnRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "dialog stream turn request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"session_id", "creature_id", "text"});
    if (!fields.isSuccess())
        return Result<DialogStreamTurnRequest>{fields.getError().value()};
    auto sessionId = streamingSessionUuid(json, path, "session_id");
    auto creatureId = streamingUuid(json, path, "creature_id");
    auto text = json_codec::requiredString(json, path, "text", MAX_DIALOG_STREAM_TEXT_BYTES);
    if (!sessionId.isSuccess())
        return Result<DialogStreamTurnRequest>{sessionId.getError().value()};
    if (!creatureId.isSuccess())
        return Result<DialogStreamTurnRequest>{creatureId.getError().value()};
    if (!text.isSuccess())
        return Result<DialogStreamTurnRequest>{text.getError().value()};
    return Result<DialogStreamTurnRequest>{
        {sessionId.getValue().value(), creatureId.getValue().value(), text.getValue().value()}};
}

inline Result<DialogStreamFinishRequest> dialogStreamFinishRequestFromJson(const nlohmann::json &json) {
    constexpr std::string_view path = "dialog stream finish request";
    auto fields = json_codec::rejectUnknownFields(json, path, {"session_id"});
    if (!fields.isSuccess())
        return Result<DialogStreamFinishRequest>{fields.getError().value()};
    auto sessionId = streamingSessionUuid(json, path, "session_id");
    if (!sessionId.isSuccess())
        return Result<DialogStreamFinishRequest>{sessionId.getError().value()};
    return Result<DialogStreamFinishRequest>{{sessionId.getValue().value()}};
}

inline nlohmann::json dialogStreamStartResponseToJson(const DialogStreamStartResponse &response) {
    auto creatureIds = nlohmann::json::array();
    for (const auto &creatureId : response.creatureIds)
        creatureIds.push_back(canonicalUuidForWire(creatureId));
    return {{"session_id", response.sessionId},
            {"status", response.status},
            {"message", response.message},
            {"creature_ids", std::move(creatureIds)},
            {"stage_id", canonicalUuidForWire(response.stageId)}};
}

inline nlohmann::json dialogStreamTurnResponseToJson(const DialogStreamTurnResponse &response) {
    return {
        {"session_id", response.sessionId}, {"status", response.status}, {"turns_received", response.turnsReceived}};
}

inline nlohmann::json dialogStreamFinishResponseToJson(const DialogStreamFinishResponse &response) {
    return {{"session_id", response.sessionId},
            {"status", response.status},
            {"message", response.message},
            {"animation_id", canonicalUuidForWire(response.animationId)},
            {"last_turn_animation_id", canonicalUuidForWire(response.lastTurnAnimationId)},
            {"playback_triggered", response.playbackTriggered},
            {"exchange_status", response.exchangeStatus},
            {"parts_rendered", response.partsRendered},
            {"parts_total", response.partsTotal}};
}

} // namespace creatures::api
