#include "server/voice/MusicClient.h"

#include <algorithm>
#include <cctype>
#include <cstring>

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

Result<MusicGenerationResult> MusicClient::generateInstrumental(const std::string &apiKey, const std::string &prompt,
                                                                int64_t musicLengthMs,
                                                                const std::string &generationMode,
                                                                std::shared_ptr<OperationSpan> parentSpan,
                                                                MusicGenerationTraceContext traceContext) const {
    auto span = creatures::observability
                    ? creatures::observability->createChildOperationSpan("MusicClient.generateInstrumental", parentSpan)
                    : nullptr;
    if (span) {
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "POST");
        span->setAttribute("url.path", "/v1/music/detailed");
        span->setAttribute("music.model_id", "music_v2");
        span->setAttribute("music.output_format", "pcm_48000");
        span->setAttribute("music.generation_mode", generationMode);
        span->setAttribute("music.length_ms", musicLengthMs);
        span->setAttribute("music.prompt_length", static_cast<int64_t>(prompt.size()));
        span->setAttribute("music.force_instrumental", true);
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
    if (prompt.empty() || prompt.size() > kMaxMusicPromptBytes || musicLengthMs < kMinMusicLengthMs ||
        musicLengthMs > kMaxMusicLengthMs ||
        (generationMode != "track" && generationMode != "loop" && generationMode != "ambience")) {
        const std::string message = "Invalid ElevenLabs music generation parameters";
        recordSpanError(span, message, "InvalidData", ServerError::InvalidData);
        return Result<MusicGenerationResult>{ServerError(ServerError::InvalidData, message)};
    }

    nlohmann::json request{{"prompt", prompt},
                           {"music_length_ms", musicLengthMs},
                           {"model_id", "music_v2"},
                           {"force_instrumental", true},
                           {"generation_mode", generationMode},
                           {"store_for_inpainting", false},
                           {"with_timestamps", false},
                           {"sign_with_c2pa", false}};
    const auto requestText = request.dump();
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
    result.request = std::move(request);
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

} // namespace creatures::voice
