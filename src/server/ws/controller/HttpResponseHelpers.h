#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>
#include <oatpp/core/Types.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>

#include "api/JsonResponse.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

// Canonical values for status-response JSON. All lowercase per issue #16.
// "ok" — success (2xx).
// "error" — generic failure (4xx other than 404, plus all 5xx).
// "not_found" — 404 specifically; clients use this as a cheap discriminator so
// they don't have to parse the numeric code to distinguish a missing resource
// from a malformed request.
inline constexpr const char *STATUS_OK = api::STATUS_OK;
inline constexpr const char *STATUS_ERROR = api::STATUS_ERROR;
inline constexpr const char *STATUS_NOT_FOUND = api::STATUS_NOT_FOUND;

// Pick the canonical status string for an HTTP code when the caller hasn't
// overridden it. 404 → "not_found", 2xx → "ok", everything else → "error".
inline const char *defaultStatusForCode(int code) { return api::defaultStatusForCode(code); }

// CRTP mixin that gives an oatpp ApiController consistent error/success
// response shapes. Inherit alongside ApiController, with the controller's own
// type as Self:
//
//     class FooController : public oatpp::web::server::api::ApiController,
//                           public HttpResponseHelpers<FooController> { ... };
//
// Every helper stamps http.status_code on the request span, builds the neutral
// canonical envelope, and adapts its serialized bytes to an oat++ response.
// CRTP is needed only to reach ApiController's protected createResponse.
template <typename Self> class HttpResponseHelpers {
  protected:
    using HttpStatus = oatpp::web::protocol::http::Status;
    using HttpOutgoingResponse = oatpp::web::protocol::http::outgoing::Response;

    // Generic error-envelope helper. `statusStringOverride` is rarely needed
    // — defaultStatusForCode covers 404 → "not_found" and everything else.
    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse>
    bailHttp(const SpanT &span, const HttpStatus &status, const std::string &message,
             const char *statusStringOverride = nullptr, const char *errorType = nullptr) {
        const auto body =
            api::statusResponseToJson(api::makeStatusResponse(status.code, message, statusStringOverride));
        if (span) {
            span->setError(message);
            span->setAttribute("error.code", static_cast<int64_t>(status.code));
            span->setAttribute("error.message", message);
            if (errorType)
                span->setAttribute("error.type", errorType);
            span->setHttpStatus(status.code);
        }
        return jsonResponse(span, status, body);
    }

    // One-liner for the very common service-layer Result<T> failure pattern.
    // ServerError::Code → HTTP code via the canonical serverErrorToStatusCode
    // mapping (NotFound→404, InvalidData→400, Conflict→409, Forbidden→403,
    // everything else→500).
    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse> bailFromServerError(const SpanT &span, const creatures::ServerError &error) {
        const int code = creatures::serverErrorToStatusCode(error.getCode());
        recordSpanError(span, error.getMessage(), serverErrorType(error.getCode()), error.getCode());
        if (span)
            span->setHttpStatus(code);
        const auto body = api::statusResponseToJson(api::makeStatusResponse(code, error.getMessage()));
        return jsonResponse(span, HttpStatus(code, statusReasonForCode(code)), body);
    }

    // Success-shaped canonical JSON response — for endpoints whose only return
    // value is "yes, it worked." Status code defaults to 200 but can be any
    // 2xx (e.g. 201 Created on a POST).
    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse> okStatus(const SpanT &span, const HttpStatus &status,
                                                   const std::string &message) {
        const auto body = api::statusResponseToJson(api::makeStatusResponse(status.code, message, STATUS_OK));
        if (span)
            span->setHttpStatus(status.code);
        return jsonResponse(span, status, body);
    }

    // Serialize framework-neutral contracts without routing them back through
    // oat++ DTO wrappers. This is the temporary transport adapter until the
    // HTTP framework itself is replaced.
    std::shared_ptr<HttpOutgoingResponse> jsonResponse(const HttpStatus &status, const nlohmann::json &body) {
        return serializedJsonResponse(status, api::serializeJson(body).bytes);
    }

    template <typename SpanT>
    std::shared_ptr<HttpOutgoingResponse> jsonResponse(const SpanT &span, const HttpStatus &status,
                                                       const nlohmann::json &body) {
        const auto serialization = api::serializeJson(body);
        if (span) {
            span->setAttribute("http.response.body.size", static_cast<int64_t>(serialization.bytes.size()));
            span->setAttribute("http.response.content_type", "application/json");
            span->setAttribute("response.codec", "nlohmann_json");
            span->setAttribute("response.utf8.replaced", serialization.invalidUtf8Replaced);
        }
        return serializedJsonResponse(status, serialization.bytes);
    }

  private:
    std::shared_ptr<HttpOutgoingResponse> serializedJsonResponse(const HttpStatus &status,
                                                                 const std::string &serialized) {
        auto response = static_cast<Self *>(this)->createResponse(status, serialized.c_str());
        response->putHeader("Content-Type", "application/json; charset=utf-8");
        return response;
    }

    static const char *serverErrorType(creatures::ServerError::Code code) {
        switch (code) {
        case creatures::ServerError::NotFound:
            return "NotFound";
        case creatures::ServerError::Forbidden:
            return "Forbidden";
        case creatures::ServerError::InvalidData:
            return "InvalidData";
        case creatures::ServerError::DatabaseError:
            return "DatabaseError";
        case creatures::ServerError::Conflict:
            return "Conflict";
        default:
            return "InternalError";
        }
    }

    // bailFromServerError builds a Status from a numeric code, but oatpp's
    // Status constructor needs a reason phrase too. This covers the codes
    // serverErrorToStatusCode actually returns; anything else gets "Unknown"
    // (which is fine because the response body carries the real message).
    static const char *statusReasonForCode(int code) {
        switch (code) {
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 409:
            return "Conflict";
        case 500:
            return "Internal Server Error";
        default:
            return "Unknown";
        }
    }
};

} // namespace creatures::ws
