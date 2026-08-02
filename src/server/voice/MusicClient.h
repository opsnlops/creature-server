#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::voice {

inline constexpr int64_t kMinMusicLengthMs = 3000;
inline constexpr int64_t kMaxMusicLengthMs = 600000;
inline constexpr int64_t kMaxMusicDurationExtensionMs = 60000;
inline constexpr std::size_t kMaxMusicPromptBytes = 4100;

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

class MusicClient {
  public:
    Result<MusicGenerationResult> generateInstrumental(const std::string &apiKey, const std::string &prompt,
                                                       int64_t musicLengthMs, const std::string &generationMode,
                                                       std::shared_ptr<OperationSpan> parentSpan = nullptr,
                                                       MusicGenerationTraceContext traceContext = {}) const;

    /// Public for a focused multipart parser test; production callers use
    /// generateInstrumental.
    static Result<MusicGenerationResult> parseDetailedResponse(const std::vector<uint8_t> &body,
                                                               const std::string &contentType, int rawPcmChannels = 2);
};

} // namespace creatures::voice
