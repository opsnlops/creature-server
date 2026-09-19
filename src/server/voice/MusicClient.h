#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "server/voice/MusicTypes.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

struct MusicGenerationResult {
    std::vector<uint8_t> audioPcm;
    int sourceChannels{0};
    nlohmann::json request;
    nlohmann::json responseMetadata;
    nlohmann::json compositionPlan;
    nlohmann::json songMetadata;
    std::string songId;
    std::string requestId;
};

/// Business identifiers copied onto the external-call span so an upstream
/// request can be found directly without first expanding its parent trace.
struct MusicGenerationTraceContext {
    std::string jobId;
    std::string musicGenerationId;
    std::string dialogGenerationId;
    std::string scriptId;
    int64_t dialogDurationMs{0};
    int64_t durationExtensionMs{0};
};

/// `POST /v1/music/plan` result: the plan JSON is passed through verbatim so
/// the console edits exactly what ElevenLabs proposed.
struct MusicPlanResult {
    nlohmann::json compositionPlan;
    std::string requestId;
};

struct MusicFinetune {
    std::string id;
    std::string name;
    std::string modelId;
    std::string status;
    std::string visibility;
    std::string createdBy;
    std::optional<std::string> primaryGenre;
    std::vector<std::string> tags;
    double trainingProgress{0.0};
};

class MusicClient {
  public:
    /// `POST /v1/music/detailed` in either prompt or composition-plan mode.
    /// The request is validated at the API boundary; this re-checks the
    /// invariants cheaply so a programming error can't reach ElevenLabs.
    Result<MusicGenerationResult> generate(const std::string &apiKey, const MusicGenerationRequest &request,
                                           std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                           MusicGenerationTraceContext traceContext = {}) const;

    /// `POST /v1/music/plan`: prompt + exact length → editable composition
    /// plan. `sourcePlan` seeds the new plan from an existing one.
    Result<MusicPlanResult> generatePlan(const std::string &apiKey, const std::string &prompt, int64_t musicLengthMs,
                                         const std::string &modelId, const nlohmann::json &sourcePlan = nullptr,
                                         std::shared_ptr<OperationSpan> parentSpan = nullptr) const;

    /// `GET /v1/music/finetunes` (first page, 150 max).
    Result<std::vector<MusicFinetune>> listFinetunes(const std::string &apiKey,
                                                     std::shared_ptr<OperationSpan> parentSpan = nullptr) const;

    /// Public for focused parser tests; production callers use generatePlan /
    /// listFinetunes.
    static Result<MusicPlanResult> parsePlanResponse(const std::string &body);
    static Result<std::vector<MusicFinetune>> parseFinetunesResponse(const std::string &body);

    /// Public for a focused multipart parser test; production callers use
    /// generate.
    static Result<MusicGenerationResult> parseDetailedResponse(const std::vector<uint8_t> &body,
                                                               const std::string &contentType, int rawPcmChannels = 2);
};

} // namespace creatures::voice
