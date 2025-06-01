#pragma once

#include <string>
#include <memory>
#include <unordered_map>

#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/tracer.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/context/context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>

namespace creatures {

// Forward declarations
class RequestSpan;
class OperationSpan;

/**
 * A wrapper around OpenTelemetry for the Creature Server
 *
 * This class provides a clean interface for creating traces and spans
 * without tightly coupling the rest of the codebase to OTel APIs.
 */
class ObservabilityManager {
public:
    ObservabilityManager();
    ~ObservabilityManager() = default;

    /**
     * Initialize the OpenTelemetry provider with Honeycomb endpoint
     *
     * @param serviceName The name of this service (e.g., "creature-server")
     * @param serviceVersion The version string
     * @param honeycombApiKey Your Honeycomb API key
     * @param honeycombDataset The Honeycomb dataset name
     */
    void initialize(const std::string& serviceName,
                   const std::string& serviceVersion,
                   const std::string& honeycombApiKey = "",
                   const std::string& honeycombDataset = "creature-server");

    /**
     * Create a new span for an HTTP request
     *
     * @param operationName The name of the operation (e.g., "GET /api/v1/animation")
     * @param httpMethod The HTTP method (GET, POST, etc.)
     * @param httpUrl The full URL path
     * @return A unique pointer to the span (RAII cleanup)
     */
    std::unique_ptr<RequestSpan> createRequestSpan(const std::string& operationName,
                                                  const std::string& httpMethod,
                                                  const std::string& httpUrl);

    /**
     * Create a child span for database operations, service calls, etc.
     *
     * @param operationName The name of the operation
     * @param parentSpan The parent span (optional)
     * @return A unique pointer to the span
     */
    std::unique_ptr<OperationSpan> createOperationSpan(const std::string& operationName,
                                                       std::unique_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Create a child operation span from another operation span
     *
     * @param operationName The name of the operation
     * @param parentSpan The parent operation span
     * @return A unique pointer to the span
     */
    std::unique_ptr<OperationSpan> createChildOperationSpan(const std::string& operationName,
                                                            std::unique_ptr<OperationSpan> parentSpan = nullptr);

private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer_;
    bool initialized_;
};

/**
 * RAII wrapper for request spans with automatic status setting
 */
class RequestSpan {
public:
    RequestSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
               const std::string& httpMethod,
               const std::string& httpUrl);
    ~RequestSpan();

    // No copy, move only
    RequestSpan(const RequestSpan&) = delete;
    RequestSpan& operator=(const RequestSpan&) = delete;
    RequestSpan(RequestSpan&&) = default;
    RequestSpan& operator=(RequestSpan&&) = default;

    /**
     * Set the HTTP response status
     */
    void setHttpStatus(int statusCode);

    /**
     * Add an attribute to the span
     */
    void setAttribute(const std::string& key, const std::string& value);
    void setAttribute(const std::string& key, int64_t value);
    void setAttribute(const std::string& key, bool value);

    /**
     * Record an exception that occurred during processing
     */
    void recordException(const std::exception& ex);

    /**
     * Get the underlying span for creating child spans
     */
    [[nodiscard]] opentelemetry::trace::Span* getSpan() const { return span_.get(); }

    /**
     * Get the span context for parent-child relationships
     */
    [[nodiscard]] opentelemetry::context::Context getContext() const { return context_; }

    // Make span_ accessible to ObservabilityManager for parent-child relationships
    friend class ObservabilityManager;

private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    opentelemetry::context::Context context_;
    bool statusSet_;
};

/**
 * RAII wrapper for operation spans (database calls, etc.)
 */
class OperationSpan {
public:
    explicit OperationSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span);
    ~OperationSpan();

    // No copy, move only
    OperationSpan(const OperationSpan&) = delete;
    OperationSpan& operator=(const OperationSpan&) = delete;
    OperationSpan(OperationSpan&&) = default;
    OperationSpan& operator=(OperationSpan&&) = default;

    /**
     * Mark the operation as successful
     */
    void setSuccess();

    /**
     * Mark the operation as failed with an error message
     */
    void setError(const std::string& errorMessage);

    /**
     * Add attributes
     */
    void setAttribute(const std::string& key, const std::string& value);
    void setAttribute(const std::string& key, int64_t value);
    void setAttribute(const std::string& key, bool value);

    /**
     * Record an exception
     */
    void recordException(const std::exception& ex);

    /**
     * Get the span context for parent-child relationships
     */
    [[nodiscard]] opentelemetry::context::Context getContext() const { return context_; }

    /**
     * Get the underlying span for creating child spans
     */
    [[nodiscard]] opentelemetry::trace::Span* getSpan() const { return span_.get(); }

private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    opentelemetry::context::Context context_;
    bool statusSet_;
};

// Convenience macro for creating request spans
#define CREATE_REQUEST_SPAN(observability_manager, method, url) \
    auto span = observability_manager->createRequestSpan(method + " " + url, method, url)

} // namespace creatures