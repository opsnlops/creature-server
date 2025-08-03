#pragma once

#include "exception/exception.h"
#include "spdlog/spdlog.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <fmt/format.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>

namespace creatures {

class JsonParser {
  public:
    /**
     * Safely convert a BSON document to nlohmann::json with comprehensive error handling
     *
     * @param view The BSON document view to convert
     * @param context Context string for error messages (e.g., "creature abc123", "animation xyz789")
     * @param span Optional observability span for tracing
     * @return Result containing the parsed JSON or an error
     */
    static Result<nlohmann::json> bsonToJson(const bsoncxx::document::view &view, const std::string &context,
                                             std::shared_ptr<OperationSpan> span = nullptr);

    /**
     * Safely parse a JSON string with comprehensive error handling
     *
     * @param jsonString The JSON string to parse
     * @param context Context string for error messages
     * @param span Optional observability span for tracing
     * @return Result containing the parsed JSON or an error
     */
    static Result<nlohmann::json> parseJsonString(const std::string &jsonString, const std::string &context,
                                                  std::shared_ptr<OperationSpan> span = nullptr);

    /**
     * Safely convert JSON string to BSON document with comprehensive error handling
     *
     * @param jsonString The JSON string to convert to BSON
     * @param context Context string for error messages
     * @param span Optional observability span for tracing
     * @return Result containing the BSON document or an error
     */
    static Result<bsoncxx::document::value> jsonStringToBson(const std::string &jsonString, const std::string &context,
                                                             std::shared_ptr<OperationSpan> span = nullptr);

  private:
    static void setSpanError(std::shared_ptr<OperationSpan> span, const std::string &errorType,
                             const std::string &errorMessage, const std::exception &e);
};

} // namespace creatures