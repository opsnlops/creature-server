#include "ErrorHandler.h"

#include <string>
#include <string_view>

#include "api/JsonResponse.h"

namespace creatures ::ws {

namespace {

// oatpp reports JSON failures by prefixing the message with the internal
// function that raised it. Which function it was tells us whose fault it is:
//
//   Deserializer::*  and  Utils::*   -> reading the REQUEST. The client sent
//                                       something unparseable. That's a 400.
//   Serializer::*                    -> writing OUR response. Genuinely our
//                                       fault, and must stay a 500.
//
// Before #129 only a few Deserializer spellings were recognised, so a body
// that failed earlier — at the preparse/tokenize step, e.g. an unterminated
// string — escaped as a 500. Same class of fault, two different answers, and
// a 500 tells a caller to retry or page someone when they should be fixing
// their request.
constexpr std::string_view kClientJsonFaultMarkers[] = {
    "oatpp::parser::json::mapping::Deserializer::",
    "oatpp::parser::json::Utils::",
};
constexpr std::string_view kServerJsonFaultMarker = "oatpp::parser::json::mapping::Serializer::";

bool looksLikeMalformedRequestJson(const std::string &message) {
    if (message.find(kServerJsonFaultMarker) != std::string::npos) {
        return false; // our own serialization broke; that really is a 500
    }
    for (const auto marker : kClientJsonFaultMarkers) {
        if (message.find(marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

// Strip oatpp's "[oatpp::parser::json::...]: " prefix but keep everything
// after it — the tail carries the useful part ("Unknown field 'foo'",
// "'\"' - expected"). The internal function name is noise to a caller and
// shouldn't be in a response body.
std::string withoutInternalPrefix(const std::string &message) {
    if (!message.empty() && message.front() == '[') {
        const auto close = message.find("]: ");
        if (close != std::string::npos) {
            return message.substr(close + 3);
        }
    }
    return message;
}

} // namespace

ErrorHandler::ErrorHandler(const std::shared_ptr<oatpp::data::mapping::ObjectMapper> &objectMapper) {
    // Kept in the constructor until AppComponent no longer owns an oat++
    // object mapper. Error responses themselves are framework-neutral now.
    static_cast<void>(objectMapper);
}

std::shared_ptr<ErrorHandler::OutgoingResponse>
ErrorHandler::handleError(const Status &status, const oatpp::String &message, const Headers &headers) {

    Status actualStatus = status;
    oatpp::String actualMessage = message;

    if (message && status.code == 500) {
        const std::string msgStr = std::string(message);
        if (looksLikeMalformedRequestJson(msgStr)) {
            actualStatus = Status::CODE_400;
            actualMessage = oatpp::String(("Invalid request body: " + withoutInternalPrefix(msgStr)).c_str());
        }
    }

    const auto error =
        api::makeStatusResponse(actualStatus.code, actualMessage ? std::string(actualMessage) : std::string{});
    const auto serialized = api::jsonToString(api::statusResponseToJson(error));
    auto response = ResponseFactory::createResponse(actualStatus, serialized.c_str());
    response->putHeader("Content-Type", "application/json; charset=utf-8");

    for (const auto &pair : headers.getAll()) {
        response->putHeader(pair.first.toString(), pair.second.toString());
    }

    return response;
}

} // namespace creatures::ws
