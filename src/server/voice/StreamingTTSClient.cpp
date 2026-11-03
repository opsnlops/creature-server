#include "StreamingTTSClient.h"

#include <cstring>
#include <exception>
#include <utility>

#include <base64.hpp>
#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include "ElevenLabsHttp.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

StreamingTTSClient::StreamingTTSClient() = default;

StreamingTTSClient::~StreamingTTSClient() = default;

namespace {
using elevenlabs_http::appendToString;
using elevenlabs_http::checkResponse;
using elevenlabs_http::ElevenLabsCall;
using elevenlabs_http::kNoBodyForLog;
} // namespace

Result<StreamingTTSResult> StreamingTTSClient::generateSpeechREST(const std::string &apiKey, const std::string &voiceId,
                                                                  const std::string &modelId, const std::string &text,
                                                                  const std::string &outputFormat, float stability,
                                                                  float similarityBoost,
                                                                  const std::vector<std::string> &previousRequestIds,
                                                                  ProgressCallback progressCallback,
                                                                  std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability->createChildOperationSpan("StreamingTTSClient.generateSpeechREST", parentSpan);
    if (span) {
        span->setAttribute("voice.id", voiceId);
        span->setAttribute("voice.model", modelId);
        span->setAttribute("text.length", static_cast<int64_t>(text.size()));
        span->setAttribute("audio.format", outputFormat);
        span->setAttribute("previous_request_ids.count", static_cast<int64_t>(previousRequestIds.size()));
    }

    if (progressCallback) {
        progressCallback(0.05f);
    }

    // Build JSON request body
    nlohmann::json body;
    body["text"] = text;
    body["model_id"] = modelId;
    body["voice_settings"]["stability"] = stability;
    body["voice_settings"]["similarity_boost"] = similarityBoost;
    if (!previousRequestIds.empty()) {
        body["previous_request_ids"] = previousRequestIds;
    }
    std::string bodyStr = body.dump();

    // Build URL
    std::string url =
        fmt::format("https://api.elevenlabs.io/v1/text-to-speech/{}/stream/with-timestamps?output_format={}", voiceId,
                    outputFormat);

    // Result accumulation — curl callbacks append here
    StreamingTTSResult result;
    result.audioFormat = outputFormat;
    int chunkCount = 0;
    std::string lineBuffer; // Accumulates partial lines across curl callbacks

    // Curl write callback: receives response body data
    struct WriteContext {
        StreamingTTSResult *result;
        int *chunkCount;
        std::string *lineBuffer;
    };
    WriteContext writeCtx{&result, &chunkCount, &lineBuffer};

    auto writeCallback = [](char *data, size_t size, size_t nmemb, void *userdata) -> size_t {
        auto *ctx = static_cast<WriteContext *>(userdata);
        size_t totalBytes = size * nmemb;

        // Append to line buffer
        ctx->lineBuffer->append(data, totalBytes);

        // Process all complete newline-delimited JSON lines
        size_t pos = 0;
        while (pos < ctx->lineBuffer->size()) {
            auto nlPos = ctx->lineBuffer->find('\n', pos);
            if (nlPos == std::string::npos) {
                // Partial line — keep remainder for next callback
                *ctx->lineBuffer = ctx->lineBuffer->substr(pos);
                return totalBytes;
            }

            std::string line = ctx->lineBuffer->substr(pos, nlPos - pos);
            pos = nlPos + 1;

            // Trim
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (line.empty())
                continue;

            try {
                auto json = nlohmann::json::parse(line);

                // Decode audio
                if (json.contains("audio_base64") && json["audio_base64"].is_string()) {
                    auto audioB64 = json["audio_base64"].get<std::string>();
                    if (!audioB64.empty()) {
                        try {
                            auto decoded = base64::from_base64(audioB64);
                            ctx->result->audioData.insert(
                                ctx->result->audioData.end(), reinterpret_cast<const uint8_t *>(decoded.data()),
                                reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
                            (*ctx->chunkCount)++;
                        } catch (...) {
                            // Base64 decode failed — skip this chunk
                        }
                    }
                }

                // Parse alignment (seconds → milliseconds)
                if (json.contains("alignment") && !json["alignment"].is_null()) {
                    auto &al = json["alignment"];
                    if (al.contains("characters") && al.contains("character_start_times_seconds") &&
                        al.contains("character_end_times_seconds")) {
                        auto chars = al["characters"].get<std::vector<std::string>>();
                        auto starts = al["character_start_times_seconds"].get<std::vector<double>>();
                        auto ends = al["character_end_times_seconds"].get<std::vector<double>>();

                        size_t count = std::min({chars.size(), starts.size(), ends.size()});
                        for (size_t i = 0; i < count; ++i) {
                            TextToViseme::CharTiming ct;
                            ct.character = chars[i].empty() ? ' ' : chars[i][0];
                            ct.startTimeMs = starts[i] * 1000.0;
                            ct.durationMs = (ends[i] - starts[i]) * 1000.0;
                            ctx->result->charTimings.push_back(ct);
                            ctx->result->alignmentText.push_back(ct.character);
                        }
                    }
                }
            } catch (const nlohmann::json::exception &) {
                // Not valid JSON — skip
            }
        }

        ctx->lineBuffer->clear();
        return totalBytes;
    };

    if (progressCallback) {
        progressCallback(0.10f);
    }

    ElevenLabsCall call(apiKey, url);
    if (!call.initOk()) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }
    call.addHeader("Content-Type: application/json");
    call.addHeader("Accept: text/event-stream");
    curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(call.handle(), CURLOPT_WRITEFUNCTION,
                     static_cast<size_t (*)(char *, size_t, size_t, void *)>(writeCallback));
    curl_easy_setopt(call.handle(), CURLOPT_WRITEDATA, &writeCtx);
    curl_easy_setopt(call.handle(), CURLOPT_TIMEOUT, 30L);

    long httpCode = 0;
    const CURLcode res = call.perform(httpCode);
    result.requestId = call.requestId();
    call.recordTimings(span);

    if (auto err = checkResponse<StreamingTTSResult>(res, httpCode, "ElevenLabs REST TTS", kNoBodyForLog, span)) {
        return *err;
    }

    // Process any remaining data in the line buffer
    if (!lineBuffer.empty()) {
        lineBuffer.push_back('\n');
        writeCallback(lineBuffer.data(), 1, lineBuffer.size(), &writeCtx);
    }

    // Estimate duration
    result.audioDurationSeconds = estimateAudioSeconds(outputFormat, result.audioData.size());

    info("StreamingTTSClient REST complete: {} chunks, {} bytes audio, {} alignment chars, "
         "request_id={}, {:.2f}s estimated",
         chunkCount, result.audioData.size(), result.charTimings.size(), result.requestId, result.audioDurationSeconds);

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.charTimings.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setAttribute("request_id", result.requestId);
        span->setAttribute("chunks", static_cast<int64_t>(chunkCount));
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    return result;
}

} // namespace creatures::voice
