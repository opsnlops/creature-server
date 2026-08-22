#include "JsonParser.h"

namespace creatures {

namespace {

inline constexpr int MAX_API_JSON_DEPTH = 32;

class JsonDepthError final : public std::runtime_error {
  public:
    JsonDepthError() : std::runtime_error("JSON nesting exceeds maximum depth 32") {}
};

} // namespace

Result<nlohmann::json> JsonParser::bsonToJson(const bsoncxx::document::view &view, const std::string &context,
                                              std::shared_ptr<OperationSpan> span) {
    try {
        // First convert BSON to JSON string
        std::string bson_json_str = bsoncxx::to_json(view);
        debug("BSON document converted to JSON string for {} (length: {})", context, bson_json_str.length());

        // Then parse the JSON string
        nlohmann::json result = nlohmann::json::parse(bson_json_str);
        debug("JSON parsing successful for {}", context);

        if (span) {
            span->setSuccess();
            span->setAttribute("json.size_bytes", static_cast<int64_t>(result.dump().length()));
            span->setAttribute("bson_json.size_bytes", static_cast<int64_t>(bson_json_str.length()));
        }

        return Result<nlohmann::json>{result};

    } catch (const nlohmann::json::parse_error &e) {
        std::string errorMessage = fmt::format("JSON parse error for {}: {} at byte {}", context, e.what(), e.byte);
        critical(errorMessage);
        setSpanError(span, "JSONParseError", errorMessage, e);
        return Result<nlohmann::json>{ServerError(ServerError::DatabaseError, errorMessage)};

    } catch (const bsoncxx::exception &e) {
        std::string errorMessage = fmt::format("BSON conversion error for {}: {}", context, e.what());
        critical(errorMessage);
        setSpanError(span, "BSONConversionError", errorMessage, e);
        return Result<nlohmann::json>{ServerError(ServerError::DatabaseError, errorMessage)};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Unexpected error during JSON conversion for {}: {}", context, e.what());
        critical(errorMessage);
        setSpanError(span, "UnexpectedError", errorMessage, e);
        return Result<nlohmann::json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<nlohmann::json> JsonParser::parseJsonString(const std::string &jsonString, const std::string &context,
                                                   std::shared_ptr<OperationSpan> span) {
    try {
        nlohmann::json result = nlohmann::json::parse(jsonString);
        debug("JSON parsing successful for {}", context);

        if (span) {
            span->setSuccess();
            span->setAttribute("json.size_bytes", static_cast<int64_t>(result.dump().length()));
        }

        return Result<nlohmann::json>{result};

    } catch (const nlohmann::json::parse_error &e) {
        std::string errorMessage = fmt::format("JSON parse error for {}: {} at byte {}", context, e.what(), e.byte);
        critical(errorMessage);
        setSpanError(span, "JSONParseError", errorMessage, e);
        return Result<nlohmann::json>{ServerError(ServerError::DatabaseError, errorMessage)};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Unexpected error during JSON parsing for {}: {}", context, e.what());
        critical(errorMessage);
        setSpanError(span, "UnexpectedError", errorMessage, e);
        return Result<nlohmann::json>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

Result<nlohmann::json> JsonParser::parseApiJsonString(const std::string &jsonString, const std::string &context,
                                                      std::shared_ptr<OperationSpan> span) {
    try {
        auto depthCallback = [](int depth, nlohmann::json::parse_event_t, nlohmann::json &) {
            if (depth > MAX_API_JSON_DEPTH) {
                throw JsonDepthError{};
            }
            return true;
        };
        auto result = nlohmann::json::parse(jsonString, depthCallback);
        if (span) {
            span->setAttribute("json.size_bytes", static_cast<int64_t>(jsonString.length()));
            span->setSuccess();
        }
        return Result<nlohmann::json>{std::move(result)};
    } catch (const nlohmann::json::parse_error &exception) {
        auto message = fmt::format("JSON parse error for {}: {} at byte {}", context, exception.what(), exception.byte);
        warn(message);
        setSpanError(span, "JSONParseError", message, exception);
        if (span) {
            span->setError(message);
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<nlohmann::json>{ServerError(ServerError::InvalidData, message)};
    } catch (const JsonDepthError &exception) {
        auto message = fmt::format("JSON parse error for {}: {}", context, exception.what());
        warn(message);
        setSpanError(span, "JSONDepthError", message, exception);
        if (span) {
            span->setError(message);
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InvalidData));
        }
        return Result<nlohmann::json>{ServerError(ServerError::InvalidData, message)};
    } catch (const std::exception &exception) {
        auto message = fmt::format("Unexpected error during API JSON parsing for {}: {}", context, exception.what());
        error(message);
        setSpanError(span, "UnexpectedError", message, exception);
        if (span) {
            span->setError(message);
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
        }
        return Result<nlohmann::json>{ServerError(ServerError::InternalError, message)};
    }
}

Result<bsoncxx::document::value> JsonParser::jsonStringToBson(const std::string &jsonString, const std::string &context,
                                                              std::shared_ptr<OperationSpan> span) {
    try {
        bsoncxx::document::value bsonDoc = bsoncxx::from_json(jsonString);
        debug("JSON string converted to BSON for {}", context);

        if (span) {
            span->setSuccess();
            span->setAttribute("json.size_bytes", static_cast<int64_t>(jsonString.length()));
            span->setAttribute("bson.size_bytes", static_cast<int64_t>(bsonDoc.view().length()));
        }

        return Result<bsoncxx::document::value>{std::move(bsonDoc)};

    } catch (const bsoncxx::exception &e) {
        std::string errorMessage = fmt::format("BSON conversion error for {}: {}", context, e.what());
        critical(errorMessage);
        setSpanError(span, "BSONConversionError", errorMessage, e);
        return Result<bsoncxx::document::value>{ServerError(ServerError::DatabaseError, errorMessage)};

    } catch (const std::exception &e) {
        std::string errorMessage = fmt::format("Unexpected error during BSON conversion for {}: {}", context, e.what());
        critical(errorMessage);
        setSpanError(span, "UnexpectedError", errorMessage, e);
        return Result<bsoncxx::document::value>{ServerError(ServerError::InternalError, errorMessage)};
    }
}

void JsonParser::setSpanError(std::shared_ptr<OperationSpan> span, const std::string &errorType,
                              const std::string &errorMessage, const std::exception &e) {
    if (span) {
        span->recordException(e);
        span->setAttribute("error.type", errorType);
        span->setAttribute("error.message", errorMessage);

        // Add byte position for JSON parse errors
        if (auto parseError = dynamic_cast<const nlohmann::json::parse_error *>(&e)) {
            span->setAttribute("error.byte_position", static_cast<int64_t>(parseError->byte));
        }
    }
}

} // namespace creatures
