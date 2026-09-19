#include "server/voice/MusicClient.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <string_view>

#include <curl/curl.h>
#include <fmt/format.h>

#include "server/namespace-stuffs.h"
#include "server/voice/ElevenLabsHttp.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

using elevenlabs_http::checkResponse;
using elevenlabs_http::ElevenLabsCall;

constexpr std::size_t kMaxMusicResponseBytes = 80ULL * 1024 * 1024;

size_t appendToBytes(char *data, size_t size, size_t count, void *userdata) {
    auto *bytes = static_cast<std::vector<uint8_t> *>(userdata);
    const auto length = size * count;
    if (bytes->size() + length > kMaxMusicResponseBytes) {
        return 0;
    }
    bytes->insert(bytes->end(), reinterpret_cast<uint8_t *>(data), reinterpret_cast<uint8_t *>(data) + length);
    return length;
}

std::string boundaryFromContentType(const std::string &contentType) {
    std::string lowerContentType = contentType;
    std::transform(lowerContentType.begin(), lowerContentType.end(), lowerContentType.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    auto marker = lowerContentType.find("boundary=");
    if (marker == std::string::npos) {
        return {};
    }
    auto value = contentType.substr(marker + 9);
    const auto semicolon = value.find(';');
    if (semicolon != std::string::npos) {
        value.resize(semicolon);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        value = value.substr(1, value.size() - 2);
    }
    return value;
}

uint16_t readU16(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) | (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t readU32(const std::vector<uint8_t> &bytes, std::size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) | (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

struct NormalizedPcm {
    std::vector<uint8_t> monoAudio;
    int sourceChannels{0};
};

Result<NormalizedPcm> downmixPcm(std::vector<uint8_t> audio, int channels) {
    if (channels == 1) {
        return Result<NormalizedPcm>{NormalizedPcm{std::move(audio), 1}};
    }
    if (channels != 2 || audio.size() % (2 * sizeof(int16_t)) != 0) {
        return Result<NormalizedPcm>{
            ServerError(ServerError::InvalidData, "ElevenLabs music returned invalid PCM channel data")};
    }
    std::vector<uint8_t> mono(audio.size() / 2);
    for (std::size_t source = 0, destination = 0; source < audio.size(); source += 4, destination += 2) {
        const auto left = static_cast<int16_t>(readU16(audio, source));
        const auto right = static_cast<int16_t>(readU16(audio, source + 2));
        const auto mixed = static_cast<int16_t>((static_cast<int32_t>(left) + static_cast<int32_t>(right)) / 2);
        mono[destination] = static_cast<uint8_t>(mixed & 0xff);
        mono[destination + 1] = static_cast<uint8_t>((static_cast<uint16_t>(mixed) >> 8) & 0xff);
    }
    return Result<NormalizedPcm>{NormalizedPcm{std::move(mono), 2}};
}

Result<NormalizedPcm> normalizePcm(std::vector<uint8_t> audio, int rawPcmChannels) {
    if (audio.size() >= 12 && std::memcmp(audio.data(), "RIFF", 4) == 0 &&
        std::memcmp(audio.data() + 8, "WAVE", 4) == 0) {
        int wavChannels = 0;
        bool validFormat = false;
        std::size_t offset = 12;
        while (offset + 8 <= audio.size()) {
            const uint32_t size = readU32(audio, offset + 4);
            const std::size_t dataOffset = offset + 8;
            if (dataOffset + size > audio.size()) {
                return Result<NormalizedPcm>{
                    ServerError(ServerError::InvalidData, "ElevenLabs music returned a truncated WAV")};
            }
            if (std::memcmp(audio.data() + offset, "fmt ", 4) == 0 && size >= 16) {
                wavChannels = readU16(audio, dataOffset + 2);
                validFormat = readU16(audio, dataOffset) == 1 && (wavChannels == 1 || wavChannels == 2) &&
                              readU32(audio, dataOffset + 4) == 48000 && readU16(audio, dataOffset + 14) == 16;
            } else if (std::memcmp(audio.data() + offset, "data", 4) == 0) {
                if (!validFormat) {
                    return Result<NormalizedPcm>{ServerError(
                        ServerError::InvalidData, "ElevenLabs music WAV was not mono/stereo 48 kHz 16-bit PCM")};
                }
                return downmixPcm(std::vector<uint8_t>(audio.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                                                       audio.begin() + static_cast<std::ptrdiff_t>(dataOffset + size)),
                                  wavChannels);
            }
            offset = dataOffset + size + (size % 2);
        }
        return Result<NormalizedPcm>{ServerError(ServerError::InvalidData, "ElevenLabs music WAV had no data chunk")};
    }
    if (audio.empty() || audio.size() % sizeof(int16_t) != 0) {
        return Result<NormalizedPcm>{
            ServerError(ServerError::InvalidData, "ElevenLabs music returned invalid PCM bytes")};
    }
    return downmixPcm(std::move(audio), rawPcmChannels);
}

} // namespace

Result<MusicGenerationResult> MusicClient::parseDetailedResponse(const std::vector<uint8_t> &body,
                                                                 const std::string &contentType, int rawPcmChannels) {
    const auto boundary = boundaryFromContentType(contentType);
    if (boundary.empty()) {
        return Result<MusicGenerationResult>{
            ServerError(ServerError::InvalidData, "ElevenLabs detailed music response had no multipart boundary")};
    }
    const std::string raw(reinterpret_cast<const char *>(body.data()), body.size());
    const std::string delimiter = "--" + boundary;
    MusicGenerationResult result;
    bool foundJson = false;
    bool foundAudio = false;
    std::size_t cursor = 0;
    while (true) {
        const auto marker = raw.find(delimiter, cursor);
        if (marker == std::string::npos) {
            break;
        }
        auto partStart = marker + delimiter.size();
        if (raw.compare(partStart, 2, "--") == 0) {
            break;
        }
        if (raw.compare(partStart, 2, "\r\n") == 0) {
            partStart += 2;
        }
        const auto headerEnd = raw.find("\r\n\r\n", partStart);
        if (headerEnd == std::string::npos) {
            break;
        }
        const auto next = raw.find("\r\n" + delimiter, headerEnd + 4);
        if (next == std::string::npos) {
            break;
        }
        const auto headers = raw.substr(partStart, headerEnd - partStart);
        auto lowerHeaders = headers;
        std::transform(lowerHeaders.begin(), lowerHeaders.end(), lowerHeaders.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto payloadStart = headerEnd + 4;
        if (lowerHeaders.find("application/json") != std::string::npos) {
            try {
                const auto metadata = nlohmann::json::parse(raw.substr(payloadStart, next - payloadStart));
                result.responseMetadata = metadata;
                result.compositionPlan = metadata.value("composition_plan", nlohmann::json::object());
                result.songMetadata = metadata.value("song_metadata", nlohmann::json::object());
                foundJson = true;
            } catch (const std::exception &e) {
                return Result<MusicGenerationResult>{ServerError(
                    ServerError::InvalidData, fmt::format("ElevenLabs music metadata was invalid JSON: {}", e.what()))};
            }
        } else {
            std::vector<uint8_t> audio(body.begin() + static_cast<std::ptrdiff_t>(payloadStart),
                                       body.begin() + static_cast<std::ptrdiff_t>(next));
            auto pcm = normalizePcm(std::move(audio), rawPcmChannels);
            if (!pcm.isSuccess()) {
                return Result<MusicGenerationResult>{pcm.getError().value()};
            }
            auto normalized = pcm.getValue().value();
            result.audioPcm = std::move(normalized.monoAudio);
            result.sourceChannels = normalized.sourceChannels;
            foundAudio = true;
        }
        cursor = next + 2;
    }
    if (!foundJson || !foundAudio || result.audioPcm.empty()) {
        return Result<MusicGenerationResult>{
            ServerError(ServerError::InvalidData, "ElevenLabs detailed music response was missing metadata or audio")};
    }
    return Result<MusicGenerationResult>{std::move(result)};
}

namespace {

/// Shared JSON-body POST for the small music helper endpoints. The detailed
/// compose call keeps its own path because its response is multipart.
struct JsonCallOutcome {
    long httpCode{0};
    CURLcode curlResult{CURLE_OK};
    std::string body;
    std::string requestId;
};

template <typename T>
std::optional<Result<T>> performJsonCall(ElevenLabsCall &call, const char *method, const std::string &requestText,
                                         long timeoutSeconds, const std::string &whatFailed,
                                         const std::shared_ptr<OperationSpan> &span, JsonCallOutcome &outcome) {
    std::vector<uint8_t> response;
    if (std::string_view(method) == "POST") {
        call.addHeader("Content-Type: application/json");
        curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDS, requestText.c_str());
        curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDSIZE, static_cast<long>(requestText.size()));
    } else {
        curl_easy_setopt(call.handle(), CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(call.handle(), CURLOPT_WRITEFUNCTION, &appendToBytes);
    curl_easy_setopt(call.handle(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(call.handle(), CURLOPT_TIMEOUT, timeoutSeconds);
    outcome.curlResult = call.perform(outcome.httpCode);
    call.recordTimings(span);
    outcome.body.assign(response.begin(), response.end());
    outcome.requestId = call.requestId();
    if (span) {
        span->setAttribute("http.response.status_code", static_cast<int64_t>(outcome.httpCode));
        span->setAttribute("http.response.body.size", static_cast<int64_t>(response.size()));
        span->setAttribute("curl.result_code", static_cast<int64_t>(outcome.curlResult));
        if (!outcome.requestId.empty())
            span->setAttribute("elevenlabs.request_id", outcome.requestId);
    }
    const std::string errorBody = outcome.httpCode >= 200 && outcome.httpCode < 300 ? std::string{} : outcome.body;
    if (auto error = checkResponse<T>(outcome.curlResult, outcome.httpCode, whatFailed, errorBody, span)) {
        recordSpanError(span, error->getError()->getMessage(),
                        outcome.curlResult == CURLE_OK ? "ElevenLabsHttpError" : "CurlError",
                        error->getError()->getCode());
        return error;
    }
    return std::nullopt;
}

} // namespace

Result<MusicGenerationResult> MusicClient::generate(const std::string &apiKey, const MusicGenerationRequest &request,
                                                    std::shared_ptr<OperationSpan> parentSpan,
                                                    MusicGenerationTraceContext traceContext) const {
    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("MusicClient.generate", parentSpan)
                    : nullptr;
    const bool planMode = request.isPlanMode();
    if (span) {
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "POST");
        span->setAttribute("url.path", "/v1/music/detailed");
        span->setAttribute("music.model_id", request.modelId);
        span->setAttribute("music.output_format", "pcm_48000");
        span->setAttribute("music.request_kind", request.requestKind());
        span->setAttribute("music.store_for_inpainting", request.storeForInpainting);
        span->setAttribute("music.finetune_present", request.finetuneId.has_value());
        if (planMode) {
            span->setAttribute("music.chunk_count", static_cast<int64_t>(request.compositionPlan->chunks.size()));
            span->setAttribute("music.length_ms", request.compositionPlan->totalDurationMs());
            span->setAttribute("music.seed_present", request.seed.has_value());
        } else {
            span->setAttribute("music.generation_mode", request.generationMode);
            span->setAttribute("music.length_ms", request.musicLengthMs);
            span->setAttribute("music.prompt_length", static_cast<int64_t>(request.prompt.size()));
            span->setAttribute("music.force_instrumental", request.forceInstrumental);
        }
        if (!traceContext.jobId.empty())
            span->setAttribute("job.id", traceContext.jobId);
        if (!traceContext.musicGenerationId.empty())
            span->setAttribute("music.generation_id", traceContext.musicGenerationId);
        if (!traceContext.dialogGenerationId.empty())
            span->setAttribute("dialog.generation_id", traceContext.dialogGenerationId);
        if (!traceContext.scriptId.empty())
            span->setAttribute("dialog.script_id", traceContext.scriptId);
        span->setAttribute("dialog.duration_ms", traceContext.dialogDurationMs);
        span->setAttribute("music.duration_extension_ms", traceContext.durationExtensionMs);
    }
    bool valid = isSupportedMusicModelId(request.modelId);
    if (planMode) {
        const auto &plan = *request.compositionPlan;
        valid = valid && !plan.chunks.empty() && plan.chunks.size() <= kMaxMusicPlanChunks &&
                plan.totalDurationMs() >= kMinMusicLengthMs && plan.totalDurationMs() <= kMaxMusicLengthMs &&
                request.prompt.empty();
    } else {
        valid = valid && !request.prompt.empty() && request.prompt.size() <= kMaxMusicPromptBytes &&
                request.musicLengthMs >= kMinMusicLengthMs && request.musicLengthMs <= kMaxMusicLengthMs &&
                isSupportedMusicGenerationMode(request.generationMode) && !request.seed;
    }
    if (!valid) {
        const std::string message = "Invalid ElevenLabs music generation parameters";
        recordSpanError(span, message, "InvalidData", ServerError::InvalidData);
        return Result<MusicGenerationResult>{ServerError(ServerError::InvalidData, message)};
    }

    nlohmann::json requestJson = musicGenerationRequestToElevenLabsJson(request);
    const auto requestText = requestJson.dump();
    const std::string url = "https://api.elevenlabs.io/v1/music/detailed?output_format=pcm_48000";
    ElevenLabsCall call(apiKey, url);
    if (!call.initOk()) {
        const std::string message = "Failed to initialize ElevenLabs music HTTP client";
        recordSpanError(span, message, "CurlInitializationError", ServerError::InternalError);
        return Result<MusicGenerationResult>{ServerError(ServerError::InternalError, message)};
    }
    call.addHeader("Content-Type: application/json");
    std::vector<uint8_t> response;
    curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDS, requestText.c_str());
    curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDSIZE, static_cast<long>(requestText.size()));
    curl_easy_setopt(call.handle(), CURLOPT_WRITEFUNCTION, &appendToBytes);
    curl_easy_setopt(call.handle(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(call.handle(), CURLOPT_TIMEOUT, 660L);

    long httpCode = 0;
    const auto curlResult = call.perform(httpCode);
    call.recordTimings(span);
    if (span) {
        span->setAttribute("http.response.status_code", static_cast<int64_t>(httpCode));
        span->setAttribute("http.response.body.size", static_cast<int64_t>(response.size()));
        span->setAttribute("curl.result_code", static_cast<int64_t>(curlResult));
        span->setAttribute("elevenlabs.request_id", call.requestId());
    }
    const std::string errorBody =
        httpCode >= 200 && httpCode < 300 ? std::string{} : std::string(response.begin(), response.end());
    if (auto error = checkResponse<MusicGenerationResult>(curlResult, httpCode, "ElevenLabs music", errorBody, span)) {
        recordSpanError(span, error->getError()->getMessage(),
                        curlResult == CURLE_OK ? "ElevenLabsHttpError" : "CurlError", error->getError()->getCode());
        return *error;
    }
    auto parsed = parseDetailedResponse(response, call.contentType());
    if (!parsed.isSuccess()) {
        recordSpanError(span, parsed.getError().value().getMessage(), "ElevenLabsResponseParseError",
                        parsed.getError().value().getCode());
        return parsed;
    }
    auto result = parsed.getValue().value();
    result.request = std::move(requestJson);
    result.songId = call.songId();
    result.requestId = call.requestId();
    if (result.requestId.empty() && result.responseMetadata.is_object()) {
        result.requestId = result.responseMetadata.value("request_id", std::string{});
    }
    if (span) {
        span->setAttribute("music.song_id", result.songId);
        span->setAttribute("elevenlabs.request_id_present", !result.requestId.empty());
        if (!result.requestId.empty())
            span->setAttribute("elevenlabs.request_id", result.requestId);
        span->setAttribute("music.source_channels", static_cast<int64_t>(result.sourceChannels));
        span->setAttribute("music.pcm_bytes", static_cast<int64_t>(result.audioPcm.size()));
        span->setSuccess();
    }
    return Result<MusicGenerationResult>{std::move(result)};
}

Result<MusicPlanResult> MusicClient::parsePlanResponse(const std::string &body) {
    nlohmann::json plan;
    try {
        plan = nlohmann::json::parse(body);
    } catch (const std::exception &e) {
        return Result<MusicPlanResult>{
            ServerError(ServerError::InvalidData, fmt::format("ElevenLabs music plan was invalid JSON: {}", e.what()))};
    }
    // v2/v2.5 plans are chunk-based. A section-based `MusicPrompt` here means
    // the call fell back to music_v1, which the compose call would then reject.
    if (!plan.is_object() || !plan.contains("chunks") || !plan["chunks"].is_array() || plan["chunks"].empty()) {
        return Result<MusicPlanResult>{
            ServerError(ServerError::InvalidData, "ElevenLabs music plan did not contain any chunks")};
    }
    MusicPlanResult result;
    result.compositionPlan = std::move(plan);
    return Result<MusicPlanResult>{std::move(result)};
}

Result<MusicPlanResult> MusicClient::generatePlan(const std::string &apiKey, const std::string &prompt,
                                                  int64_t musicLengthMs, const std::string &modelId,
                                                  const nlohmann::json &sourcePlan,
                                                  std::shared_ptr<OperationSpan> parentSpan) const {
    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("MusicClient.generatePlan", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "POST");
        span->setAttribute("url.path", "/v1/music/plan");
        span->setAttribute("music.model_id", modelId);
        span->setAttribute("music.length_ms", musicLengthMs);
        span->setAttribute("music.prompt_length", static_cast<int64_t>(prompt.size()));
        span->setAttribute("music.source_plan_present", sourcePlan.is_object());
    }
    if (prompt.empty() || prompt.size() > kMaxMusicPromptBytes || musicLengthMs < kMinMusicLengthMs ||
        musicLengthMs > kMaxMusicLengthMs || !isSupportedMusicModelId(modelId)) {
        const std::string message = "Invalid ElevenLabs music plan parameters";
        recordSpanError(span, message, "InvalidData", ServerError::InvalidData);
        return Result<MusicPlanResult>{ServerError(ServerError::InvalidData, message)};
    }
    // model_id is mandatory here: the endpoint defaults to music_v1 and would
    // hand back the section-based plan shape that v2/v2.5 reject.
    nlohmann::json request{{"prompt", prompt}, {"music_length_ms", musicLengthMs}, {"model_id", modelId}};
    if (sourcePlan.is_object()) {
        request["source_composition_plan"] = sourcePlan;
    }
    ElevenLabsCall call(apiKey, "https://api.elevenlabs.io/v1/music/plan");
    if (!call.initOk()) {
        const std::string message = "Failed to initialize ElevenLabs music HTTP client";
        recordSpanError(span, message, "CurlInitializationError", ServerError::InternalError);
        return Result<MusicPlanResult>{ServerError(ServerError::InternalError, message)};
    }
    JsonCallOutcome outcome;
    if (auto error = performJsonCall<MusicPlanResult>(call, "POST", request.dump(), 120L, "ElevenLabs music plan", span,
                                                      outcome)) {
        return *error;
    }
    auto parsed = parsePlanResponse(outcome.body);
    if (!parsed.isSuccess()) {
        recordSpanError(span, parsed.getError().value().getMessage(), "ElevenLabsResponseParseError",
                        parsed.getError().value().getCode());
        return parsed;
    }
    auto result = parsed.getValue().value();
    result.requestId = outcome.requestId;
    if (span) {
        span->setAttribute("music.chunk_count", static_cast<int64_t>(result.compositionPlan["chunks"].size()));
        span->setSuccess();
    }
    return Result<MusicPlanResult>{std::move(result)};
}

Result<std::vector<MusicFinetune>> MusicClient::parseFinetunesResponse(const std::string &body) {
    using ListResult = Result<std::vector<MusicFinetune>>;
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body);
    } catch (const std::exception &e) {
        return ListResult{ServerError(ServerError::InvalidData,
                                      fmt::format("ElevenLabs music finetunes were invalid JSON: {}", e.what()))};
    }
    if (!json.is_object() || !json.contains("finetunes") || !json["finetunes"].is_array()) {
        return ListResult{ServerError(ServerError::InvalidData, "ElevenLabs music finetunes response had no list")};
    }
    std::vector<MusicFinetune> finetunes;
    for (const auto &item : json["finetunes"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            continue;
        }
        MusicFinetune finetune;
        finetune.id = item["id"].get<std::string>();
        finetune.name = item.value("name", std::string{});
        finetune.modelId = item.value("model_id", std::string{});
        finetune.status = item.value("status", std::string{});
        finetune.visibility = item.value("visibility", std::string{});
        finetune.createdBy = item.value("created_by", std::string{});
        if (item.contains("primary_genre") && item["primary_genre"].is_string()) {
            finetune.primaryGenre = item["primary_genre"].get<std::string>();
        }
        if (item.contains("tags") && item["tags"].is_array()) {
            for (const auto &tag : item["tags"]) {
                if (tag.is_string()) {
                    finetune.tags.push_back(tag.get<std::string>());
                }
            }
        }
        if (item.contains("training_progress") && item["training_progress"].is_number()) {
            finetune.trainingProgress = item["training_progress"].get<double>();
        }
        finetunes.push_back(std::move(finetune));
    }
    return ListResult{std::move(finetunes)};
}

Result<std::vector<MusicFinetune>> MusicClient::listFinetunes(const std::string &apiKey,
                                                              std::shared_ptr<OperationSpan> parentSpan) const {
    using ListResult = Result<std::vector<MusicFinetune>>;
    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("MusicClient.listFinetunes", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "GET");
        span->setAttribute("url.path", "/v1/music/finetunes");
    }
    ElevenLabsCall call(apiKey, "https://api.elevenlabs.io/v1/music/finetunes?page_size=150");
    if (!call.initOk()) {
        const std::string message = "Failed to initialize ElevenLabs music HTTP client";
        recordSpanError(span, message, "CurlInitializationError", ServerError::InternalError);
        return ListResult{ServerError(ServerError::InternalError, message)};
    }
    JsonCallOutcome outcome;
    if (auto error = performJsonCall<std::vector<MusicFinetune>>(call, "GET", {}, 30L, "ElevenLabs music finetunes",
                                                                 span, outcome)) {
        return *error;
    }
    auto parsed = parseFinetunesResponse(outcome.body);
    if (!parsed.isSuccess()) {
        recordSpanError(span, parsed.getError().value().getMessage(), "ElevenLabsResponseParseError",
                        parsed.getError().value().getCode());
        return parsed;
    }
    if (span) {
        span->setAttribute("music.finetune_count", static_cast<int64_t>(parsed.getValue().value().size()));
        span->setSuccess();
    }
    return parsed;
}

} // namespace creatures::voice
