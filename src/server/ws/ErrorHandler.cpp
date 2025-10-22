
#include "ErrorHandler.h"

namespace creatures ::ws {

ErrorHandler::ErrorHandler(const std::shared_ptr<oatpp::data::mapping::ObjectMapper> &objectMapper)
    : m_objectMapper(objectMapper) {}

std::shared_ptr<ErrorHandler::OutgoingResponse>
ErrorHandler::handleError(const Status &status, const oatpp::String &message, const Headers &headers) {

    auto error = StatusDto::createShared();
    error->status = "ERROR";

    // Check if this is a deserialization error (should be 400, not 500)
    Status actualStatus = status;
    oatpp::String actualMessage = message;

    if (message && status.code == 500) {
        std::string msgStr = std::string(message);

        // Detect oatpp deserialization errors and convert to 400 Bad Request
        if (msgStr.find("Deserializer") != std::string::npos ||
            msgStr.find("Unknown field") != std::string::npos ||
            msgStr.find("readObject()") != std::string::npos) {

            actualStatus = Status::CODE_400;

            // Provide helpful error message with required fields
            if (msgStr.find("Unknown field") != std::string::npos) {
                actualMessage = "Invalid request body. Request contains unknown field(s). Expected fields: sound_file (required)";
            } else {
                actualMessage = "Invalid request body. Missing required field(s): sound_file";
            }
        }
    }

    error->code = actualStatus.code;
    error->message = actualMessage;

    auto response = ResponseFactory::createResponse(actualStatus, error, m_objectMapper);

    for (const auto &pair : headers.getAll()) {
        response->putHeader(pair.first.toString(), pair.second.toString());
    }

    return response;
}

} // namespace creatures::ws