#pragma once

#include <memory>
#include <string>

#include <oatpp/core/Types.hpp>
#include <oatpp/web/protocol/http/Http.hpp>
#include <oatpp/web/protocol/http/outgoing/Response.hpp>

#include "server/ws/dto/StatusDto.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::ws {

// Canonical values for StatusDto.status. All lowercase per issue #16.
// "ok" — success (2xx).
// "error" — generic failure (4xx other than 404, plus all 5xx).
// "not_found" — 404 specifically; clients use this as a cheap discriminator so
// they don't have to parse the numeric code to distinguish a missing resource
// from a malformed request.
inline constexpr const char *STATUS_OK = "ok";
inline constexpr const char *STATUS_ERROR = "error";
inline constexpr const char *STATUS_NOT_FOUND = "not_found";

// Pick the canonical status string for an HTTP code when the caller hasn't
// overridden it. 404 → "not_found", 2xx → "ok", everything else → "error".
inline const char *defaultStatusForCode(int code) {
    if (code == 404)
        return STATUS_NOT_FOUND;
    if (code >= 200 && code < 300)
        return STATUS_OK;
    return STATUS_ERROR;
}

// Build the canonical StatusDto envelope. Doesn't touch any oatpp response
// machinery so it's trivially unit-testable.
inline oatpp::Object<StatusDto> buildStatusDto(int code, const std::string &message,
                                               const char *statusStringOverride = nullptr) {
    auto dto = StatusDto::createShared();
    dto->status = statusStringOverride ? statusStringOverride : defaultStatusForCode(code);
    dto->code = static_cast<v_uint16>(code);
    dto->message = message.c_str();
    return dto;
}

// CRTP mixin that gives an oatpp ApiController consistent error/success
// response shapes. Inherit alongside ApiController, with the controller's own
// type as Self:
//
//     class FooController : public oatpp::web::server::api::ApiController,
//                           public HttpResponseHelpers<FooController> { ... };
//
// Every helper (1) stamps http.status_code on the request span if one is
// passed, (2) builds a StatusDto with the canonical envelope, and (3) returns
// the OutgoingResponse via the derived controller's createDtoResponse — which
// is the only reason this needs CRTP in the first place (createDtoResponse is
// a protected member of ApiController, so a free function can't reach it).
template <typename Self> class HttpResponseHelpers {
  protected:
    using Status = oatpp::web::protocol::http::Status;
    using OutgoingResponse = oatpp::web::protocol::http::outgoing::Response;

    // Generic error-envelope helper. `statusStringOverride` is rarely needed
    // — defaultStatusForCode covers 404 → "not_found" and everything else.
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> bailHttp(const SpanT &span, const Status &status, const std::string &message,
                                               const char *statusStringOverride = nullptr) {
        auto dto = buildStatusDto(status.code, message, statusStringOverride);
        if (span)
            span->setHttpStatus(status.code);
        return static_cast<Self *>(this)->createDtoResponse(status, dto);
    }

    // One-liner for the very common service-layer Result<T> failure pattern.
    // ServerError::Code → HTTP code via the canonical serverErrorToStatusCode
    // mapping (NotFound→404, InvalidData→400, Conflict→409, Forbidden→403,
    // everything else→500).
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> bailFromServerError(const SpanT &span, const creatures::ServerError &error) {
        const int code = creatures::serverErrorToStatusCode(error.getCode());
        return bailHttp(span, Status(code, statusReasonForCode(code)), error.getMessage());
    }

    // Success-shaped StatusDto response — for endpoints whose only return
    // value is "yes, it worked." Status code defaults to 200 but can be any
    // 2xx (e.g. 201 Created on a POST).
    template <typename SpanT>
    std::shared_ptr<OutgoingResponse> okStatus(const SpanT &span, const Status &status, const std::string &message) {
        auto dto = buildStatusDto(status.code, message, STATUS_OK);
        if (span)
            span->setHttpStatus(status.code);
        return static_cast<Self *>(this)->createDtoResponse(status, dto);
    }

  private:
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
