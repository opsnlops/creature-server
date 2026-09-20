#pragma once

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "api/JsonResponse.h"
#include "server/transport/HttpTypes.h"
#include "util/JsonParser.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::transport {

/**
 * Shared response and parsing helpers for uWS route handlers. Every handler
 * file used to carry its own copy of these; keeping one set means the error
 * envelope, the span attributes, and the parse child span read the same on
 * every route, and the same as the oat++ controllers they replace.
 */

/// The oat++ HttpResponseHelpers error-type string for a ServerError code.
const char *serverErrorTypeName(ServerError::Code code);

/// A canonical status envelope. Codes >= 400 mark the span as failed.
PreparedResponse errorStatus(int statusCode, std::string message, const std::shared_ptr<OperationSpan> &span,
                             const char *errorType = "HttpError");

/// A ServerError mapped through serverErrorToStatusCode, with the span
/// attributes bailFromServerError sets on the oat++ side.
PreparedResponse serverErrorStatus(const ServerError &error, const std::shared_ptr<OperationSpan> &span);

/// A success-shaped status envelope (200 by default).
PreparedResponse okStatus(int statusCode, std::string message, const std::shared_ptr<OperationSpan> &span);

std::shared_ptr<OperationSpan> childSpan(const std::string &name, const std::shared_ptr<OperationSpan> &parent);

/// The rule the oat++ SoundController enforced on every user-supplied sound
/// name: one predictable ASCII basename with an extension and no path
/// components of any kind. Forbidden on failure, with the same messages.
Result<std::string> sanitizeSoundFilename(const std::string &filename);

/**
 * Parse an API body through the depth-bounded JSON parser and a contract
 * parser, under a child span that records the contract name and outcome,
 * exactly as the oat++ controllers' parseBody helpers did.
 */
template <typename Parser>
auto parseBody(const std::string &body, const char *contract, const char *what, Parser &&parser,
               const std::shared_ptr<OperationSpan> &span) -> decltype(parser(nlohmann::json{})) {
    using ResultT = decltype(parser(nlohmann::json{}));
    auto parseSpan = childSpan(std::string("transport.parse.") + contract, span);
    if (parseSpan)
        parseSpan->setAttribute("validation.contract", contract);
    const auto json = JsonParser::parseApiJsonString(body, what, parseSpan);
    if (!json.isSuccess()) {
        if (parseSpan)
            parseSpan->setAttribute("validation.result", "rejected");
        return ResultT{json.getError().value()};
    }
    auto parsed = parser(json.getValue().value());
    if (!parsed.isSuccess()) {
        const auto error = parsed.getError().value();
        if (parseSpan)
            parseSpan->setAttribute("validation.result", "rejected");
        recordSpanError(parseSpan, error.getMessage(), "InvalidRequest", error.getCode());
        return parsed;
    }
    if (parseSpan) {
        parseSpan->setAttribute("validation.result", "accepted");
        parseSpan->setSuccess();
    }
    return parsed;
}

} // namespace creatures::transport
