#include "StreamingTTSClient.h"

#include <chrono>
#include <cstring>
#include <exception>
#include <utility>

#include <base64.hpp>
#include <curl/curl.h>
#include <fmt/format.h>
#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include "ElevenLabsHttp.h"
#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

/// Long enough for a cold TLS handshake across the public internet.
constexpr auto kConnectTimeout = std::chrono::seconds(15);

/// An upper bound on pathology rather than a budget. The old hand-rolled
/// receive loop had no timeout at all: a server that accepted the connection
/// and then went quiet would park a render thread forever.
constexpr auto kStreamTimeout = std::chrono::seconds(120);

} // namespace

StreamingTTSClient::StreamingTTSClient() = default;

StreamingTTSClient::~StreamingTTSClient() = default;

void StreamingTTSClient::onFrame(const std::string &payload, const bool binary) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (binary) {
        pending_.audioData.insert(pending_.audioData.end(), payload.begin(), payload.end());
        return;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(payload);
    } catch (const std::exception &e) {
        error("StreamingTTSClient: unparseable frame ({} bytes): {} preview='{}'", payload.size(), e.what(),
              payload.substr(0, 200));
        return;
    }

    const auto keys = wireKeys(WireDialect::TextToSpeech);

    // Checked FIRST, and nothing else is read from this frame. That is the
    // long-standing behaviour of this path, which drives the live show; whether
    // the final frame also carries audio worth keeping is a real question, but
    // not one to answer while swapping the transport underneath it.
    if (json.contains(keys.final) && json[keys.final].is_boolean() && json[keys.final].get<bool>()) {
        info("StreamingTTSClient: {} received, {} audio bytes, {} alignment chars", keys.final,
             pending_.audioData.size(), pending_.charTimings.size());
        complete_ = true;
        cv_.notify_all();
        return;
    }

    if (json.contains("audio") && json["audio"].is_string()) {
        const auto encoded = json["audio"].get<std::string>();
        if (!encoded.empty()) {
            try {
                const auto decoded = base64::from_base64(encoded);
                pending_.audioData.insert(pending_.audioData.end(), reinterpret_cast<const uint8_t *>(decoded.data()),
                                          reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
            } catch (const std::exception &e) {
                error("StreamingTTSClient: base64 decode failed ({} chars): {}", encoded.size(), e.what());
            }
        }
    }

    if (json.contains("alignment") && !json["alignment"].is_null()) {
        appendAlignmentChunk(json["alignment"], WireDialect::TextToSpeech, pending_.charTimings, pending_.alignmentText,
                             alignmentOffsetMs_);
    }
}

Result<StreamingTTSResult> StreamingTTSClient::generateSpeech(const std::string &apiKey, const std::string &voiceId,
                                                              const std::string &modelId, const std::string &text,
                                                              const std::string &outputFormat, float stability,
                                                              float similarityBoost, ProgressCallback progressCallback,
                                                              std::shared_ptr<OperationSpan> parentSpan) {

    auto span = creatures::observability ? creatures::observability->createChildOperationSpan(
                                               "StreamingTTSClient.generateSpeech", std::move(parentSpan))
                                         : nullptr;
    if (span) {
        span->setAttribute("voice.id", voiceId);
        span->setAttribute("voice.model", modelId);
        span->setAttribute("text.length", static_cast<int64_t>(text.size()));
        span->setAttribute("audio.format", outputFormat);
    }

    if (progressCallback) {
        progressCallback(0.05f);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_ = StreamingTTSResult{};
        pending_.audioFormat = outputFormat;
        complete_ = false;
        open_ = false;
        failure_.clear();
        alignmentOffsetMs_ = 0.0;
    }

    ix::WebSocket socket;
    socket.setUrl(fmt::format("wss://api.elevenlabs.io/v1/text-to-speech/{}/stream-input"
                              "?model_id={}&output_format={}&sync_alignment=true",
                              voiceId, modelId, outputFormat));
    // Belt and braces, as the hand-rolled version was: the key goes on the
    // handshake AND in the BOS message below.
    socket.setExtraHeaders({{"xi-api-key", apiKey}});
    // One sentence per connection, so a reconnect would silently start a second
    // stream rather than resume this one.
    socket.disableAutomaticReconnection();
    socket.setOnMessageCallback([this](const ix::WebSocketMessagePtr &msg) {
        switch (msg->type) {
        case ix::WebSocketMessageType::Message:
            onFrame(msg->str, msg->binary);
            break;
        case ix::WebSocketMessageType::Open: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = true;
            cv_.notify_all();
            break;
        }
        case ix::WebSocketMessageType::Close: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
            // A close after the stream finished is the normal ending.
            if (!complete_ && failure_.empty()) {
                failure_ = "text-to-speech socket closed before the stream finished";
            }
            cv_.notify_all();
            break;
        }
        case ix::WebSocketMessageType::Error: {
            std::lock_guard<std::mutex> lock(mutex_);
            open_ = false;
            failure_ = fmt::format("text-to-speech socket error: {}", msg->errorInfo.reason);
            cv_.notify_all();
            break;
        }
        default:
            break;
        }
    });

    // stop() joins IXWebSocket's thread, and its close handler calls the
    // message callback WITHOUT checking it (IXWebSocket.cpp:46), so the
    // callback is never cleared — doing so throws std::bad_function_call on
    // that thread and aborts the process.
    socket.start();

    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, kConnectTimeout, [this] { return open_ || !failure_.empty(); })) {
            lock.unlock();
            socket.stop();
            const std::string msg = "timed out opening the text-to-speech socket";
            if (span)
                span->setError(msg);
            return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
        }
        if (!open_) {
            const std::string msg = failure_.empty() ? "text-to-speech socket did not open" : failure_;
            lock.unlock();
            socket.stop();
            if (span)
                span->setError(msg);
            return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
        }
    }

    // BOS carries the voice settings and a space; the API requires both.
    nlohmann::json bos;
    bos["text"] = " ";
    bos["voice_settings"]["stability"] = stability;
    bos["voice_settings"]["similarity_boost"] = similarityBoost;
    bos["xi_api_key"] = apiKey;
    bos["try_trigger_generation"] = false;

    nlohmann::json body;
    body["text"] = text;
    body["try_trigger_generation"] = true;

    nlohmann::json eos;
    eos["text"] = "";

    for (const auto &message : {bos, body, eos}) {
        if (!socket.sendUtf8Text(message.dump()).success) {
            socket.stop();
            const std::string msg = "failed sending to the text-to-speech socket";
            if (span)
                span->setError(msg);
            return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
        }
    }

    if (progressCallback) {
        progressCallback(0.15f);
    }

    StreamingTTSResult result;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool settled = cv_.wait_for(lock, kStreamTimeout, [this] { return complete_ || !failure_.empty(); });
        if (!settled) {
            lock.unlock();
            socket.stop();
            const std::string msg = "text-to-speech stream timed out";
            if (span)
                span->setError(msg);
            return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
        }
        if (!complete_) {
            const std::string msg = failure_;
            lock.unlock();
            socket.stop();
            if (span)
                span->setError(msg);
            return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
        }
        result = pending_;
    }
    socket.stop();

    result.audioDurationSeconds = estimateAudioSeconds(outputFormat, result.audioData.size());

    info("StreamingTTSClient COMPLETE: {} audio bytes, {} alignment chars, est {:.2f}s", result.audioData.size(),
         result.charTimings.size(), result.audioDurationSeconds);

    if (result.audioData.empty()) {
        const std::string msg = "text-to-speech stream returned no audio";
        error("StreamingTTSClient: {}", msg);
        if (span)
            span->setError(msg);
        return Result<StreamingTTSResult>{ServerError(ServerError::InternalError, msg)};
    }

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.charTimings.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setSuccess();
    }

    if (progressCallback) {
        progressCallback(1.0f);
    }

    return result;
}

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
