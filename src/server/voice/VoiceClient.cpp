#include "VoiceClient.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include <curl/curl.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "util/ObservabilityManager.h"
#include "util/uuidUtils.h"

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::voice {

namespace {

constexpr auto voicesApiBaseUrl = "https://api.elevenlabs.io";
constexpr std::size_t maxJsonResponseBytes = 4 * 1024 * 1024;
constexpr long connectTimeoutSeconds = 10;
constexpr long defaultRequestTimeoutSeconds = 2 * 60;
constexpr long speechRequestTimeoutSeconds = 30 * 60;
// Mono S16/48 kHz: 128 MiB is about 23 minutes of speech and remains safely
// below the checked RIFF limit after expansion to the 17-channel WAV layout.
constexpr std::size_t maxAudioResponseBytes = 128 * 1024 * 1024;

enum class HttpMethod { Get, Post };

struct ResponseBuffer {
    std::string data;
    std::size_t limit;
    bool exceeded{false};
};

class CurlHandle {
  public:
    explicit CurlHandle(const std::string &url, long requestTimeoutSeconds = defaultRequestTimeoutSeconds)
        : curl_(curl_easy_init()) {
        if (!curl_)
            return;
        setupOk_ = setOption(CURLOPT_URL, url.c_str()) && setOption(CURLOPT_FOLLOWLOCATION, 0L) &&
                   setOption(CURLOPT_NOSIGNAL, 1L) && setOption(CURLOPT_WRITEFUNCTION, writeCallback) &&
                   setOption(CURLOPT_CONNECTTIMEOUT, connectTimeoutSeconds) &&
                   setOption(CURLOPT_TIMEOUT, requestTimeoutSeconds);
    }

    ~CurlHandle() {
        if (curl_)
            curl_easy_cleanup(curl_);
        if (headers_)
            curl_slist_free_all(headers_);
    }

    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;
    CurlHandle(CurlHandle &&) = delete;
    CurlHandle &operator=(CurlHandle &&) = delete;

    [[nodiscard]] CURL *get() const { return setupOk_ ? curl_ : nullptr; }

    bool addHeader(const std::string &header) {
        auto *updated = curl_slist_append(headers_, header.c_str());
        if (!updated) {
            setupOk_ = false;
            return false;
        }
        headers_ = updated;
        setupOk_ = setupOk_ && setOption(CURLOPT_HTTPHEADER, headers_);
        return setupOk_;
    }

  private:
    template <typename T> bool setOption(CURLoption option, T value) {
        return curl_ && curl_easy_setopt(curl_, option, value) == CURLE_OK;
    }

    static size_t writeCallback(char *data, size_t size, size_t count, ResponseBuffer *destination) {
        const auto byteCount = size * count;
        if (byteCount > destination->limit - std::min(destination->limit, destination->data.size())) {
            destination->exceeded = true;
            return 0;
        }
        destination->data.append(data, byteCount);
        return byteCount;
    }

    CURL *curl_{nullptr};
    curl_slist *headers_{nullptr};
    bool setupOk_{false};
};

Result<std::string> performRequest(CurlHandle &handle, const std::string &apiKey, HttpMethod method,
                                   const std::string &body, std::size_t maxResponseBytes,
                                   const std::shared_ptr<OperationSpan> &span) {
    if (!handle.get()) {
        const std::string message = "Unable to initialize CURL";
        recordSpanError(span, message, "CurlInitializationError", ServerError::InternalError);
        return Result<std::string>{ServerError(ServerError::InternalError, message)};
    }

    ResponseBuffer response{{}, maxResponseBytes};
    if (!handle.addHeader(fmt::format("xi-api-key: {}", apiKey))) {
        const std::string message = "Unable to configure CURL request headers";
        recordSpanError(span, message, "CurlConfigurationError", ServerError::InternalError);
        return Result<std::string>{ServerError(ServerError::InternalError, message)};
    }
    if (curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &response) != CURLE_OK) {
        const std::string message = "Unable to configure CURL response handling";
        recordSpanError(span, message, "CurlConfigurationError", ServerError::InternalError);
        return Result<std::string>{ServerError(ServerError::InternalError, message)};
    }
    if (method == HttpMethod::Post) {
        if (curl_easy_setopt(handle.get(), CURLOPT_POST, 1L) != CURLE_OK ||
            curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDS, body.c_str()) != CURLE_OK ||
            curl_easy_setopt(handle.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(body.size())) !=
                CURLE_OK) {
            const std::string message = "Unable to configure CURL request body";
            recordSpanError(span, message, "CurlConfigurationError", ServerError::InternalError);
            return Result<std::string>{ServerError(ServerError::InternalError, message)};
        }
    }

    const auto curlResult = curl_easy_perform(handle.get());
    long statusCode = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &statusCode);
    if (span) {
        span->setAttribute("http.response.status_code", static_cast<int64_t>(statusCode));
        span->setAttribute("http.response.body.size", static_cast<int64_t>(response.data.size()));
        span->setAttribute("curl.result_code", static_cast<int64_t>(curlResult));
    }
    if (curlResult != CURLE_OK) {
        if (response.exceeded) {
            const std::string message = "ElevenLabs response exceeded the configured size limit";
            recordSpanError(span, message, "ResponseSizeLimitExceeded", ServerError::InvalidData);
            return Result<std::string>{ServerError(ServerError::InvalidData, message)};
        }
        const auto message = fmt::format("ElevenLabs request failed: {}", curl_easy_strerror(curlResult));
        recordSpanError(span, message, "CurlError", ServerError::InternalError);
        return Result<std::string>{ServerError(ServerError::InternalError, message)};
    }

    Result<std::string> result{ServerError(ServerError::InternalError, "Unexpected HTTP response")};
    switch (statusCode) {
    case 200:
    case 201:
    case 204:
        return Result<std::string>{std::move(response.data)};
    case 400:
        result =
            Result<std::string>{ServerError(ServerError::InvalidData, "ElevenLabs rejected the request as invalid")};
        break;
    case 401:
        result =
            Result<std::string>{ServerError(ServerError::Unauthorized, "ElevenLabs rejected the configured API key")};
        break;
    case 403:
        result = Result<std::string>{ServerError(ServerError::Forbidden, "ElevenLabs rejected the configured API key")};
        break;
    case 404:
        result = Result<std::string>{ServerError(ServerError::NotFound, "ElevenLabs resource not found")};
        break;
    default:
        result = Result<std::string>{
            ServerError(ServerError::InternalError, fmt::format("ElevenLabs returned HTTP status {}", statusCode))};
        break;
    }
    const auto error = result.getError().value();
    recordSpanError(span, error.getMessage(), "ElevenLabsHttpError", error.getCode());
    return result;
}

Result<nlohmann::json> parseResponse(const std::string &response, const char *operation) {
    try {
        return Result<nlohmann::json>{nlohmann::json::parse(response)};
    } catch (const nlohmann::json::exception &error) {
        return Result<nlohmann::json>{
            ServerError(ServerError::InvalidData,
                        fmt::format("ElevenLabs {} response was invalid JSON: {}", operation, error.what()))};
    }
}

} // namespace

VoiceClient::VoiceClient(std::string apiKey) : apiKey_(std::move(apiKey)) {}

Result<std::vector<Voice>> VoiceClient::listAllAvailableVoices(std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("VoiceClient.listAllAvailableVoices",
                                                                        std::move(parentSpan))
                              : nullptr;
    if (span) {
        span->setAttribute("dependency.name", "elevenlabs");
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "GET");
        span->setAttribute("url.path", "/v1/voices");
        span->setAttribute("timeout.connect_ms", static_cast<int64_t>(connectTimeoutSeconds * 1000));
        span->setAttribute("timeout.total_ms", static_cast<int64_t>(defaultRequestTimeoutSeconds * 1000));
    }
    CurlHandle handle(fmt::format("{}/v1/voices", voicesApiBaseUrl));
    handle.addHeader("Content-Type: application/json");
    auto response = performRequest(handle, apiKey_, HttpMethod::Get, "", maxJsonResponseBytes, span);
    if (!response.isSuccess())
        return Result<std::vector<Voice>>{response.getError().value()};

    auto parsed = parseResponse(response.getValue().value(), "voices");
    if (!parsed.isSuccess()) {
        recordSpanError(span, parsed.getError()->getMessage(), "ElevenLabsResponseParseError",
                        parsed.getError()->getCode());
        return Result<std::vector<Voice>>{parsed.getError().value()};
    }

    try {
        const auto &items = parsed.getValue().value().at("voices");
        if (!items.is_array()) {
            return Result<std::vector<Voice>>{
                ServerError(ServerError::InvalidData, "ElevenLabs voices response did not contain an array")};
        }
        std::vector<Voice> voices;
        voices.reserve(items.size());
        for (const auto &item : items)
            voices.push_back(Voice{item.at("voice_id").get<std::string>(), item.at("name").get<std::string>()});
        std::ranges::sort(voices, {}, &Voice::name);
        if (span) {
            span->setAttribute("voice.count", static_cast<int64_t>(voices.size()));
            span->setSuccess();
        }
        return Result<std::vector<Voice>>{std::move(voices)};
    } catch (const nlohmann::json::exception &error) {
        const auto message = fmt::format("ElevenLabs voices response had an invalid shape: {}", error.what());
        if (span)
            span->recordException(error);
        recordSpanError(span, message, "ElevenLabsResponseShapeError", ServerError::InvalidData);
        return Result<std::vector<Voice>>{ServerError(ServerError::InvalidData, message)};
    }
}

Result<Subscription> VoiceClient::getSubscriptionStatus(std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("VoiceClient.getSubscriptionStatus",
                                                                        std::move(parentSpan))
                              : nullptr;
    if (span) {
        span->setAttribute("dependency.name", "elevenlabs");
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "GET");
        span->setAttribute("url.path", "/v1/user/subscription");
        span->setAttribute("timeout.connect_ms", static_cast<int64_t>(connectTimeoutSeconds * 1000));
        span->setAttribute("timeout.total_ms", static_cast<int64_t>(defaultRequestTimeoutSeconds * 1000));
    }
    CurlHandle handle(fmt::format("{}/v1/user/subscription", voicesApiBaseUrl));
    handle.addHeader("Content-Type: application/json");
    auto response = performRequest(handle, apiKey_, HttpMethod::Get, "", maxJsonResponseBytes, span);
    if (!response.isSuccess())
        return Result<Subscription>{response.getError().value()};

    auto parsed = parseResponse(response.getValue().value(), "subscription");
    if (!parsed.isSuccess()) {
        recordSpanError(span, parsed.getError()->getMessage(), "ElevenLabsResponseParseError",
                        parsed.getError()->getCode());
        return Result<Subscription>{parsed.getError().value()};
    }

    try {
        const auto value = parsed.getValue().value();
        Subscription subscription{value.at("tier").get<std::string>(), value.at("status").get<std::string>(),
                                  value.at("character_count").get<uint32_t>(),
                                  value.at("character_limit").get<uint32_t>()};
        if (span) {
            span->setAttribute("voice.subscription.tier", subscription.tier);
            span->setAttribute("voice.subscription.status", subscription.status);
            span->setSuccess();
        }
        return Result<Subscription>{std::move(subscription)};
    } catch (const nlohmann::json::exception &error) {
        const auto message = fmt::format("ElevenLabs subscription response had an invalid shape: {}", error.what());
        if (span)
            span->recordException(error);
        recordSpanError(span, message, "ElevenLabsResponseShapeError", ServerError::InvalidData);
        return Result<Subscription>{ServerError(ServerError::InvalidData, message)};
    }
}

Result<CreatureSpeechResponse> VoiceClient::generateCreatureSpeech(const std::filesystem::path &fileSavePath,
                                                                   const CreatureSpeechRequest &speechRequest,
                                                                   std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("VoiceClient.generateCreatureSpeech",
                                                                        std::move(parentSpan))
                              : nullptr;
    if (span) {
        span->setAttribute("dependency.name", "elevenlabs");
        span->setAttribute("server.address", "api.elevenlabs.io");
        span->setAttribute("http.request.method", "POST");
        span->setAttribute("url.path", "/v1/text-to-speech/{voice_id}");
        span->setAttribute("voice.id", speechRequest.voice_id);
        span->setAttribute("voice.model_id", speechRequest.model_id);
        span->setAttribute("speech.text.length", static_cast<int64_t>(speechRequest.text.size()));
        span->setAttribute("timeout.connect_ms", static_cast<int64_t>(connectTimeoutSeconds * 1000));
        span->setAttribute("timeout.total_ms", static_cast<int64_t>(speechRequestTimeoutSeconds * 1000));
    }
    if (fileSavePath.empty() || !std::filesystem::is_directory(fileSavePath)) {
        return Result<CreatureSpeechResponse>{
            ServerError(ServerError::InvalidData, "Speech output path must be an existing directory")};
    }
    if (speechRequest.text.empty() || speechRequest.voice_id.empty() || speechRequest.model_id.empty()) {
        return Result<CreatureSpeechResponse>{
            ServerError(ServerError::InvalidData, "Speech text, voice ID, and model ID are required")};
    }
    if (speechRequest.stability < 0.0F || speechRequest.stability > 1.0F || speechRequest.similarity_boost < 0.0F ||
        speechRequest.similarity_boost > 1.0F) {
        return Result<CreatureSpeechResponse>{
            ServerError(ServerError::InvalidData, "Voice settings must be between zero and one")};
    }

    const auto fileBaseName = makeFileName(speechRequest);
    const auto transcriptPath = fileSavePath / fmt::format("{}.txt", fileBaseName);
    std::ofstream transcriptFile(transcriptPath);
    if (!transcriptFile || !(transcriptFile << speechRequest.text)) {
        return Result<CreatureSpeechResponse>{
            ServerError(ServerError::Forbidden, "Unable to write the speech transcript")};
    }
    transcriptFile.close();

    const auto removeOutput = [](const std::filesystem::path &path) {
        std::error_code error;
        std::filesystem::remove(path, error);
    };

    const auto soundFilePath = fileSavePath / fmt::format("{}.pcm", fileBaseName);
    CurlHandle handle(
        fmt::format("{}/v1/text-to-speech/{}?output_format=pcm_48000", voicesApiBaseUrl, speechRequest.voice_id),
        speechRequestTimeoutSeconds);
    handle.addHeader("Content-Type: application/json");
    handle.addHeader("Accept: audio/pcm");
    const nlohmann::json requestJson = {
        {"text", speechRequest.text},
        {"model_id", speechRequest.model_id},
        {"voice_settings",
         {{"stability", speechRequest.stability}, {"similarity_boost", speechRequest.similarity_boost}}}};
    auto response = performRequest(handle, apiKey_, HttpMethod::Post, requestJson.dump(), maxAudioResponseBytes, span);
    if (!response.isSuccess()) {
        removeOutput(transcriptPath);
        return Result<CreatureSpeechResponse>{response.getError().value()};
    }

    const auto audio = response.getValue().value();
    std::ofstream soundFile(soundFilePath, std::ios::binary);
    if (!soundFile || !(soundFile << audio)) {
        soundFile.close();
        removeOutput(soundFilePath);
        removeOutput(transcriptPath);
        return Result<CreatureSpeechResponse>{
            ServerError(ServerError::Forbidden, "Unable to write generated speech audio")};
    }

    if (span) {
        span->setAttribute("speech.audio.size", static_cast<int64_t>(audio.size()));
        span->setSuccess();
    }
    return Result<CreatureSpeechResponse>{CreatureSpeechResponse{true, soundFilePath.filename().string(),
                                                                 transcriptPath.filename().string(),
                                                                 static_cast<uint32_t>(audio.size())}};
}

std::string VoiceClient::makeFileName(const CreatureSpeechRequest &speechRequest) {
    auto fileName = speechRequest.creature_name.empty() ? speechRequest.voice_id
                                                        : toLowerAndReplaceSpaces(speechRequest.creature_name);
    const auto now = std::chrono::system_clock::now();
    const auto nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm timeInfo{};
    gmtime_r(&nowTime, &timeInfo);
    std::ostringstream timestamp;
    timestamp << std::put_time(&timeInfo, "%Y-%m-%d_%H-%M-%S");
    fileName += fmt::format("_{}", timestamp.str());
    fileName += fmt::format("_{}", speechRequest.title.empty() ? speechRequest.model_id
                                                               : toLowerAndReplaceSpaces(speechRequest.title));
    fileName += fmt::format("_{}", util::generateUUID());
    std::replace(fileName.begin(), fileName.end(), ' ', '_');
    std::replace(fileName.begin(), fileName.end(), '/', '_');
    std::replace(fileName.begin(), fileName.end(), '\\', '_');
    return fileName;
}

std::string VoiceClient::toLowerAndReplaceSpaces(std::string value) {
    std::ranges::transform(value, value.begin(),
                           [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    std::replace(value.begin(), value.end(), ' ', '-');
    return value;
}

} // namespace creatures::voice
