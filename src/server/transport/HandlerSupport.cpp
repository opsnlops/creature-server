#include "server/transport/HandlerSupport.h"

#include <filesystem>
#include <regex>

namespace creatures {
extern std::shared_ptr<ObservabilityManager> observability;
}

namespace creatures::transport {

const char *serverErrorTypeName(const ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::Unauthorized:
        return "Unauthorized";
    case ServerError::Forbidden:
        return "Forbidden";
    case ServerError::InvalidData:
        return "InvalidData";
    case ServerError::DatabaseError:
        return "DatabaseError";
    case ServerError::Conflict:
        return "Conflict";
    default:
        return "InternalError";
    }
}

PreparedResponse errorStatus(const int statusCode, std::string message, const std::shared_ptr<OperationSpan> &span,
                             const char *errorType) {
    if (span && statusCode >= 400) {
        span->setAttribute("error.type", errorType);
        span->setAttribute("error.code", static_cast<int64_t>(statusCode));
        span->setAttribute("error.message", message);
        span->setError(message);
    }
    return PreparedResponse::json(statusCode, api::jsonToString(api::statusResponseToJson(
                                                  api::makeStatusResponse(statusCode, std::move(message)))));
}

PreparedResponse serverErrorStatus(const ServerError &error, const std::shared_ptr<OperationSpan> &span) {
    const int statusCode = serverErrorToStatusCode(error.getCode());
    if (span) {
        span->setAttribute("server.error.code", static_cast<int64_t>(error.getCode()));
    }
    return errorStatus(statusCode, error.getMessage(), span, serverErrorTypeName(error.getCode()));
}

PreparedResponse okStatus(const int statusCode, std::string message, const std::shared_ptr<OperationSpan> &span) {
    if (span)
        span->setSuccess();
    return PreparedResponse::json(statusCode, api::jsonToString(api::statusResponseToJson(api::makeStatusResponse(
                                                  statusCode, std::move(message), api::STATUS_OK))));
}

Result<std::string> sanitizeSoundFilename(const std::string &filename) {
    using ResultT = Result<std::string>;
    if (filename.empty() || filename.find('\0') != std::string::npos) {
        return ResultT{ServerError(ServerError::Forbidden, "Invalid filename: Empty or contains null bytes.")};
    }
    if (const std::filesystem::path candidate(filename); candidate.is_absolute() || candidate.has_root_path() ||
                                                         candidate.has_parent_path() ||
                                                         candidate != candidate.filename()) {
        return ResultT{ServerError(ServerError::Forbidden, "Invalid filename: Path traversal detected.")};
    }
    static const std::regex validFilenameRegex("^[a-zA-Z0-9_-]+\\.[a-zA-Z0-9]+$");
    if (!std::regex_match(filename, validFilenameRegex)) {
        return ResultT{ServerError(ServerError::Forbidden, "Invalid filename: Contains unsafe characters.")};
    }
    return ResultT{filename};
}

std::shared_ptr<OperationSpan> childSpan(const std::string &name, const std::shared_ptr<OperationSpan> &parent) {
    return creatures::observability ? creatures::observability->createChildOperationSpan(name, parent) : nullptr;
}

} // namespace creatures::transport
