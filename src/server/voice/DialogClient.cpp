#include "DialogClient.h"

#include <cstddef>
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

namespace {
using elevenlabs_http::appendToString;
using elevenlabs_http::checkResponse;
using elevenlabs_http::ElevenLabsCall;
} // namespace

std::string DialogClient::stripTags(const std::string &text) {
    // Drop "[...]" tags, then collapse runs of whitespace to single spaces, then trim.
    std::string noTags;
    noTags.reserve(text.size());
    int depth = 0;
    for (char c : text) {
        if (c == '[') {
            ++depth;
            continue;
        }
        if (c == ']') {
            if (depth > 0) {
                --depth;
            }
            continue;
        }
        if (depth == 0) {
            noTags.push_back(c);
        }
    }

    std::string collapsed;
    collapsed.reserve(noTags.size());
    bool inSpace = false;
    for (char c : noTags) {
        const bool isSpace = (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v');
        if (isSpace) {
            if (!inSpace && !collapsed.empty()) {
                collapsed.push_back(' ');
            }
            inSpace = true;
        } else {
            collapsed.push_back(c);
            inSpace = false;
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

Result<DialogResult> DialogClient::generateDialog(const std::string &apiKey, const std::vector<DialogInput> &inputs,
                                                  const std::string &outputFormat,
                                                  std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("DialogClient.generateDialog", parentSpan);
    if (span) {
        span->setAttribute("dialog.inputs", static_cast<int64_t>(inputs.size()));
        span->setAttribute("audio.format", outputFormat);
        std::size_t totalChars = 0;
        for (const auto &in : inputs) {
            totalChars += in.text.size();
        }
        span->setAttribute("dialog.total_chars", static_cast<int64_t>(totalChars));
    }

    if (inputs.empty()) {
        std::string msg = "generateDialog requires at least one input turn";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InvalidData, msg)};
    }
    if (inputs.size() > dialog_limits::kMaxDialogInputs) {
        std::string msg =
            fmt::format("generateDialog: {} turns exceeds cap of {}", inputs.size(), dialog_limits::kMaxDialogInputs);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InvalidData, msg)};
    }
    {
        std::size_t totalChars = 0;
        for (const auto &in : inputs) {
            totalChars += in.text.size();
        }
        if (totalChars > dialog_limits::kMaxDialogTotalChars) {
            std::string msg =
                fmt::format("generateDialog: total text {} chars exceeds per-call cap of {} (chunkTurns first?)",
                            totalChars, dialog_limits::kMaxDialogTotalChars);
            error(msg);
            if (span)
                span->setError(msg);
            return Result<DialogResult>{ServerError(ServerError::InvalidData, msg)};
        }
    }

    // Build the request body. Text-to-dialogue is eleven_v3-only (other models
    // are rejected by the server with HTTP 400 "does not support dialogue"). The
    // ad-hoc single-character path BLOCKLISTS eleven_v3; we deliberately bypass
    // that here — it's the whole point of this method.
    nlohmann::json body;
    body["model_id"] = "eleven_v3";
    auto inputsArr = nlohmann::json::array();
    for (const auto &in : inputs) {
        inputsArr.push_back({{"voice_id", in.voiceId}, {"text", in.text}});
    }
    body["inputs"] = std::move(inputsArr);
    const std::string bodyStr = body.dump();

    const std::string url =
        fmt::format("https://api.elevenlabs.io/v1/text-to-dialogue/with-timestamps?output_format={}", outputFormat);

    // Single-blob JSON response (NOT newline-delimited), so we accumulate to a
    // string and parse once at the end.
    std::string respBuf;
    DialogResult result;
    result.audioFormat = outputFormat;

    ElevenLabsCall call(apiKey, url);
    if (!call.initOk()) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
    }
    call.addHeader("Content-Type: application/json");
    call.addHeader("Accept: application/json");
    curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDS, bodyStr.c_str());
    curl_easy_setopt(call.handle(), CURLOPT_POSTFIELDSIZE, static_cast<long>(bodyStr.size()));
    curl_easy_setopt(call.handle(), CURLOPT_WRITEFUNCTION, &appendToString);
    curl_easy_setopt(call.handle(), CURLOPT_WRITEDATA, &respBuf);
    // Dialog is slow — eleven_v3 with forced-alignment downstream is the bottleneck.
    // 90s gives headroom for ~2000-char scenes without leaving the call open forever.
    curl_easy_setopt(call.handle(), CURLOPT_TIMEOUT, 90L);

    long httpCode = 0;
    const CURLcode res = call.perform(httpCode);
    result.requestId = call.requestId();

    if (auto err = checkResponse<DialogResult>(res, httpCode, "ElevenLabs dialog", respBuf, span)) {
        return *err;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(respBuf);
    } catch (const std::exception &e) {
        // Widened to std::exception so std::bad_alloc (from a pathological /
        // huge JSON, even one within the kMaxResponseBytes cap) doesn't
        // escape this function and crash the worker thread.
        std::string msg = fmt::format("ElevenLabs dialog: response parse failed: {}", e.what());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
    }

    // audio_base64 → bytes
    if (json.contains("audio_base64") && json["audio_base64"].is_string()) {
        try {
            const auto decoded = base64::from_base64(json["audio_base64"].get<std::string>());
            result.audioData.assign(reinterpret_cast<const uint8_t *>(decoded.data()),
                                    reinterpret_cast<const uint8_t *>(decoded.data()) + decoded.size());
        } catch (const std::exception &e) {
            std::string msg = fmt::format("ElevenLabs dialog: base64 decode failed: {}", e.what());
            error(msg);
            if (span)
                span->setError(msg);
            return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
        }
    } else {
        std::string msg = "ElevenLabs dialog: response missing audio_base64";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
    }

    // alignment.characters → kept for downstream sanity checks. The TIMES in
    // alignment are broken on eleven_v3 (confirmed empirically) — we don't expose
    // them on the result and we won't use them. Real timing comes from forcedAlignment().
    if (json.contains("alignment") && json["alignment"].is_object()) {
        const auto &al = json["alignment"];
        if (al.contains("characters") && al["characters"].is_array()) {
            try {
                result.alignmentCharacters = al["characters"].get<std::vector<std::string>>();
            } catch (const nlohmann::json::exception &) {
                // Non-string entries — treat as fatal; the downstream index math relies on this.
                std::string msg = "ElevenLabs dialog: alignment.characters was not an array of strings";
                error(msg);
                if (span)
                    span->setError(msg);
                return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
            }
        }
    }

    // voice_segments[] — char ranges + speaker. Times kept for diagnostics only.
    if (json.contains("voice_segments") && json["voice_segments"].is_array()) {
        for (const auto &seg : json["voice_segments"]) {
            DialogVoiceSegment v;
            if (seg.contains("voice_id"))
                v.voiceId = seg["voice_id"].get<std::string>();
            if (seg.contains("character_start_index"))
                v.characterStartIndex = seg["character_start_index"].get<std::size_t>();
            if (seg.contains("character_end_index"))
                v.characterEndIndex = seg["character_end_index"].get<std::size_t>();
            if (seg.contains("dialogue_input_index"))
                v.dialogInputIndex = seg["dialogue_input_index"].get<std::size_t>();
            if (seg.contains("start_time_seconds"))
                v.startTimeSeconds = seg["start_time_seconds"].get<double>();
            if (seg.contains("end_time_seconds"))
                v.endTimeSeconds = seg["end_time_seconds"].get<double>();
            result.voiceSegments.push_back(std::move(v));
        }
    }
    if (result.voiceSegments.empty()) {
        std::string msg = "ElevenLabs dialog: response missing voice_segments";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<DialogResult>{ServerError(ServerError::InternalError, msg)};
    }

    // Duration estimate. pcm_48000 is mono 16-bit @ 48 kHz → 96000 bytes/sec.
    // Generic pcm_* fallback assumes the rate in the format string.
    if (outputFormat == "pcm_48000") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 96000.0;
    } else if (outputFormat == "pcm_44100") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 88200.0;
    } else if (outputFormat == "pcm_24000") {
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 48000.0;
    } else if (outputFormat.find("mp3") != std::string::npos) {
        // Same heuristic the existing REST path uses; only approximate.
        result.audioDurationSeconds = static_cast<double>(result.audioData.size()) / 24000.0;
    }

    info("Dialog complete: {} inputs, {} bytes audio, {} alignment chars, {} segments, ~{:.2f}s, request_id={}",
         inputs.size(), result.audioData.size(), result.alignmentCharacters.size(), result.voiceSegments.size(),
         result.audioDurationSeconds, result.requestId);

    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(result.audioData.size()));
        span->setAttribute("audio.duration_s", result.audioDurationSeconds);
        span->setAttribute("alignment.chars", static_cast<int64_t>(result.alignmentCharacters.size()));
        span->setAttribute("dialog.segments", static_cast<int64_t>(result.voiceSegments.size()));
        span->setAttribute("request_id", result.requestId);
        span->setSuccess();
    }
    return result;
}

Result<ForcedAlignmentResult> DialogClient::forcedAlignment(const std::string &apiKey,
                                                            const std::vector<uint8_t> &audio,
                                                            const std::string &contentType,
                                                            const std::string &transcript,
                                                            std::shared_ptr<OperationSpan> parentSpan) {
    auto span = creatures::observability->createChildOperationSpan("DialogClient.forcedAlignment", parentSpan);
    if (span) {
        span->setAttribute("audio.bytes", static_cast<int64_t>(audio.size()));
        span->setAttribute("audio.content_type", contentType);
        span->setAttribute("transcript.length", static_cast<int64_t>(transcript.size()));
    }

    if (audio.empty()) {
        std::string msg = "forcedAlignment requires non-empty audio";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }
    if (audio.size() > dialog_limits::kMaxForcedAlignmentAudioBytes) {
        std::string msg = fmt::format("forcedAlignment: audio {} bytes exceeds cap of {}", audio.size(),
                                      dialog_limits::kMaxForcedAlignmentAudioBytes);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }
    if (transcript.empty()) {
        std::string msg = "forcedAlignment requires non-empty transcript";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }
    if (transcript.size() > dialog_limits::kMaxForcedAlignmentTranscriptBytes) {
        std::string msg = fmt::format("forcedAlignment: transcript {} bytes exceeds cap of {}", transcript.size(),
                                      dialog_limits::kMaxForcedAlignmentTranscriptBytes);
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InvalidData, msg)};
    }

    ElevenLabsCall call(apiKey, "https://api.elevenlabs.io/v1/forced-alignment");
    if (!call.initOk()) {
        std::string msg = "Failed to initialize curl";
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }
    call.addHeader("Accept: application/json");

    // Multipart body via curl's MIME API. Content-Type is set automatically by
    // CURLOPT_MIMEPOST (it includes the boundary), so we don't add one ourselves.
    curl_mime *mime = call.createMime();
    curl_mimepart *filePart = curl_mime_addpart(mime);
    curl_mime_name(filePart, "file");
    curl_mime_filename(filePart, "audio.wav");
    curl_mime_type(filePart, contentType.c_str());
    curl_mime_data(filePart, reinterpret_cast<const char *>(audio.data()), audio.size());

    curl_mimepart *textPart = curl_mime_addpart(mime);
    curl_mime_name(textPart, "text");
    curl_mime_data(textPart, transcript.data(), transcript.size());

    std::string respBuf;
    curl_easy_setopt(call.handle(), CURLOPT_WRITEFUNCTION, &appendToString);
    curl_easy_setopt(call.handle(), CURLOPT_WRITEDATA, &respBuf);
    curl_easy_setopt(call.handle(), CURLOPT_TIMEOUT, 90L);

    long httpCode = 0;
    const CURLcode res = call.perform(httpCode);

    if (auto err = checkResponse<ForcedAlignmentResult>(res, httpCode, "ElevenLabs forced-alignment", respBuf, span)) {
        return *err;
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(respBuf);
    } catch (const std::exception &e) {
        // Widened to std::exception so std::bad_alloc doesn't escape. See
        // generateDialog for the same rationale.
        std::string msg = fmt::format("ElevenLabs forced-alignment: response parse failed: {}", e.what());
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }

    ForcedAlignmentResult fa;

    auto extractEntries = [](const nlohmann::json &arr, auto &outVec, auto makeEntry) {
        for (const auto &item : arr) {
            if (!item.is_object())
                continue;
            outVec.push_back(makeEntry(item));
        }
    };

    if (json.contains("words") && json["words"].is_array()) {
        extractEntries(json["words"], fa.words, [](const nlohmann::json &it) {
            ForcedAlignmentWord w;
            if (it.contains("text"))
                w.text = it["text"].get<std::string>();
            if (it.contains("start"))
                w.startSeconds = it["start"].get<double>();
            if (it.contains("end"))
                w.endSeconds = it["end"].get<double>();
            return w;
        });
    }
    if (json.contains("characters") && json["characters"].is_array()) {
        extractEntries(json["characters"], fa.characters, [](const nlohmann::json &it) {
            ForcedAlignmentChar c;
            if (it.contains("text"))
                c.text = it["text"].get<std::string>();
            if (it.contains("start"))
                c.startSeconds = it["start"].get<double>();
            if (it.contains("end"))
                c.endSeconds = it["end"].get<double>();
            return c;
        });
    }
    if (json.contains("loss") && json["loss"].is_number()) {
        fa.loss = json["loss"].get<double>();
    }

    if (fa.characters.empty() && fa.words.empty()) {
        std::string msg = "ElevenLabs forced-alignment: response had no words or characters";
        error(msg);
        if (span)
            span->setError(msg);
        return Result<ForcedAlignmentResult>{ServerError(ServerError::InternalError, msg)};
    }

    info("Forced alignment complete: {} words, {} chars, loss={:.3f}, request_id={}", fa.words.size(),
         fa.characters.size(), fa.loss, call.requestId());

    if (span) {
        span->setAttribute("forced_alignment.words", static_cast<int64_t>(fa.words.size()));
        span->setAttribute("forced_alignment.chars", static_cast<int64_t>(fa.characters.size()));
        span->setAttribute("forced_alignment.loss", fa.loss);
        // Captured by ElevenLabsCall for every endpoint; surface it so ElevenLabs
        // support can correlate a failing alignment with the same trace.
        span->setAttribute("request_id", call.requestId());
        span->setSuccess();
    }
    return fa;
}

} // namespace creatures::voice
