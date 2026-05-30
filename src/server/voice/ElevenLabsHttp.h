#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include <curl/curl.h>
#include <fmt/format.h>

#include "server/namespace-stuffs.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

/// Shared HTTP plumbing for ElevenLabs REST calls.
///
/// Every endpoint we hit (text-to-speech with-timestamps, text-to-dialogue
/// with-timestamps, forced-alignment) uses the same envelope: xi-api-key
/// auth, request-id capture, curl rc + http-code → Result<T> mapping. This
/// header collects that machinery so it isn't duplicated per-client.
///
/// Header-only because the helpers are small and template-y. Each TU that
/// includes this gets its own internal copies (inline / inline-template),
/// which is fine — there's no state to share between TUs.

namespace creatures::voice::elevenlabs_http {

/// Header callback that captures the request-id (or x-request-id) value into
/// the std::string pointed to by `userdata`. Case-insensitive match.
inline size_t captureRequestIdHeader(char *data, size_t size, size_t nmemb, void *userdata) {
    auto *requestId = static_cast<std::string *>(userdata);
    const size_t totalBytes = size * nmemb;
    std::string header(data, totalBytes);

    std::string lower = header;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (lower.find("request-id:") == 0 || lower.find("x-request-id:") == 0) {
        const auto colonPos = header.find(':');
        if (colonPos != std::string::npos) {
            std::string value = header.substr(colonPos + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                value.erase(value.begin());
            }
            while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' ')) {
                value.pop_back();
            }
            *requestId = value;
        }
    }
    return totalBytes;
}

/// Append-to-std::string write callback for one-shot JSON responses.
inline size_t appendToString(char *data, size_t size, size_t nmemb, void *userdata) {
    auto *buf = static_cast<std::string *>(userdata);
    const size_t totalBytes = size * nmemb;
    buf->append(data, totalBytes);
    return totalBytes;
}

/// RAII wrapper for an ElevenLabs HTTP call.
///
/// Owns the curl easy handle, the headers list, and (optionally) the MIME body —
/// all freed in the destructor regardless of how the call site exits. Pre-wires
/// the standard xi-api-key auth header and the request-id capture callback so
/// every call site gets those for free. Endpoints that don't need the request-id
/// just ignore the captured value (the cost is one trivial branch per response
/// header line).
///
/// Call sites still own the request-specific bits — Content-Type, body
/// (POSTFIELDS or MIMEPOST via createMime()), write callback, and timeout —
/// so each method stays readable about what it actually sends and parses.
class ElevenLabsCall {
  public:
    ElevenLabsCall(const std::string &apiKey, const std::string &url) {
        curl_ = curl_easy_init();
        if (!curl_) {
            return;
        }
        addHeader(fmt::format("xi-api-key: {}", apiKey));
        curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl_, CURLOPT_HEADERFUNCTION, &captureRequestIdHeader);
        curl_easy_setopt(curl_, CURLOPT_HEADERDATA, &requestId_);
    }

    ~ElevenLabsCall() {
        if (mime_) {
            curl_mime_free(mime_);
        }
        if (headers_) {
            curl_slist_free_all(headers_);
        }
        if (curl_) {
            curl_easy_cleanup(curl_);
        }
    }

    ElevenLabsCall(const ElevenLabsCall &) = delete;
    ElevenLabsCall &operator=(const ElevenLabsCall &) = delete;
    ElevenLabsCall(ElevenLabsCall &&) = delete;
    ElevenLabsCall &operator=(ElevenLabsCall &&) = delete;

    [[nodiscard]] bool initOk() const { return curl_ != nullptr; }
    [[nodiscard]] CURL *handle() const { return curl_; }
    [[nodiscard]] const std::string &requestId() const { return requestId_; }

    void addHeader(const std::string &header) { headers_ = curl_slist_append(headers_, header.c_str()); }

    /// Create + own a curl_mime against this handle. Returns the handle so the
    /// caller can add parts. Ownership stays with this wrapper (freed in dtor).
    curl_mime *createMime() {
        mime_ = curl_mime_init(curl_);
        return mime_;
    }

    /// Execute. Applies any added headers and the MIME body if one was created,
    /// then performs the request. On return, `httpCode` is set from the response;
    /// the return value is the curl rc.
    CURLcode perform(long &httpCode) {
        if (headers_) {
            curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers_);
        }
        if (mime_) {
            curl_easy_setopt(curl_, CURLOPT_MIMEPOST, mime_);
        }
        const CURLcode rc = curl_easy_perform(curl_);
        httpCode = 0;
        curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &httpCode);
        return rc;
    }

  private:
    CURL *curl_ = nullptr;
    curl_slist *headers_ = nullptr;
    curl_mime *mime_ = nullptr;
    std::string requestId_;
};

/// Map curl rc + http code into a Result<T> error, or std::nullopt on HTTP 200.
///
/// - curl rc != OK → InternalError (DNS, connect, timeout, TLS, ...)
/// - HTTP 400 → InvalidData (the caller's request was malformed)
/// - any other non-200 → InternalError (upstream failure)
///
/// `whatFailed` prefixes the error message ("ElevenLabs dialog"); `bodyForLog`
/// is appended to non-200 errors so we capture the API's complaint in the log.
template <typename T>
std::optional<Result<T>> checkResponse(CURLcode rc, long httpCode, const std::string &whatFailed,
                                       const std::string &bodyForLog, const std::shared_ptr<OperationSpan> &span) {
    if (rc != CURLE_OK) {
        std::string msg = fmt::format("{} curl error: {}", whatFailed, curl_easy_strerror(rc));
        error(msg);
        if (span) {
            span->setError(msg);
        }
        return Result<T>{ServerError(ServerError::InternalError, msg)};
    }
    if (httpCode != 200) {
        std::string msg = fmt::format("{} HTTP {}: {}", whatFailed, httpCode, bodyForLog);
        error(msg);
        if (span) {
            span->setError(msg);
        }
        const auto code = (httpCode == 400) ? ServerError::InvalidData : ServerError::InternalError;
        return Result<T>{ServerError(code, msg)};
    }
    return std::nullopt;
}

/// Sentinel that means "no body to log" — used when the upstream body is the
/// streaming NDJSON which we've already consumed and trimmed.
inline constexpr const char *kNoBodyForLog = "";

} // namespace creatures::voice::elevenlabs_http
