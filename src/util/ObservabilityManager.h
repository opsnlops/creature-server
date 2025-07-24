#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include <opentelemetry/context/context.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/tracer.h>

// Metrics headers for v1.21.0
#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/sync_instruments.h>

#include "server/namespace-stuffs.h"

namespace creatures {

// Forward declarations
class RequestSpan;
class OperationSpan;

/**
 * A wrapper around OpenTelemetry for the Creature Server
 *
 * This class provides a clean interface for creating traces and spans,
 * and also handles metrics export through the existing event loop system.
 * No need to reinvent the wheel - we'll hop right into your existing pattern!
 * 🐰
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
    void initialize(const std::string &serviceName,
                    const std::string &serviceVersion,
                    const std::string &honeycombApiKey = "",
                    const std::string &honeycombDataset = "creature-server");

    /**
     * Create a new span for an HTTP request
     *
     * @param operationName The name of the operation (e.g., "GET
     * /api/v1/animation")
     * @param httpMethod The HTTP method (GET, POST, etc.)
     * @param httpUrl The full URL path
     * @return A unique pointer to the span (RAII cleanup)
     */
    std::shared_ptr<RequestSpan>
    createRequestSpan(const std::string &operationName,
                      const std::string &httpMethod,
                      const std::string &httpUrl);

    /**
     * Create a child span for database operations, service calls, etc.
     *
     * @param operationName The name of the operation
     * @param parentSpan The parent span (optional)
     * @return A unique pointer to the span
     */
    std::shared_ptr<OperationSpan>
    createOperationSpan(const std::string &operationName,
                        std::shared_ptr<RequestSpan> parentSpan = nullptr);

    /**
     * Create a child operation span from another operation span
     *
     * @param operationName The name of the operation
     * @param parentSpan The parent operation span
     * @return A unique pointer to the span
     */
    std::shared_ptr<OperationSpan> createChildOperationSpan(
        const std::string &operationName,
        std::shared_ptr<OperationSpan> parentSpan = nullptr);

    /**
     * Export all metrics from the SystemCounters to OTel
     * This is designed to be called from your existing counter-send event!
     *
     * @param metrics The SystemCounters instance containing all current metric
     * values
     */
    void exportMetrics(const std::shared_ptr<class SystemCounters> &metrics);

    /**
     * Check if the manager is initialized and ready to hop! 🐰
     */
    [[nodiscard]] bool isInitialized() const { return initialized_; }

  private:
    // Tracing members
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer_;

    // Metrics members
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter_;

    // Individual counter instruments - we'll create these once and reuse them
    // This avoids the overhead of looking them up every time we export
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        totalFramesCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        eventsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        framesStreamedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        dmxEventsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        animationsPlayedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        soundsPlayedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        playlistsStartedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        playlistsStoppedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        playlistsEventsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        playlistStatusRequestsCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        restRequestsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        rtpEventsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        soundFilesServedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        websocketConnectionsProcessedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        websocketMessagesReceivedCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        websocketMessagesSentCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        websocketPingsSentCounter_;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<uint64_t>>
        websocketPongsReceivedCounter_;

    bool initialized_;

    /**
     * Initialize all the metric instruments
     * Called once during initialize() to set up all the counters
     */
    void initializeMetricInstruments();
};

/**
 * RAII wrapper for request spans with automatic status setting
 */
class RequestSpan {
  public:
    RequestSpan(
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
        const std::string &httpMethod, const std::string &httpUrl);
    ~RequestSpan();

    // No copy, move only
    RequestSpan(const RequestSpan &) = delete;
    RequestSpan &operator=(const RequestSpan &) = delete;
    RequestSpan(RequestSpan &&) = default;
    RequestSpan &operator=(RequestSpan &&) = default;

    /**
     * Set the HTTP response status
     */
    void setHttpStatus(int statusCode);

    /**
     * Add an attribute to the span
     */
    void setAttribute(const std::string &key, const std::string &value);
    void setAttribute(const std::string &key, int64_t value);
    void setAttribute(const std::string &key, bool value);

    /**
     * Record an exception that occurred during processing
     */
    void recordException(const std::exception &ex);

    /**
     * Get the underlying span for creating child spans
     */
    [[nodiscard]] opentelemetry::trace::Span *getSpan() const {
        return span_.get();
    }

    /**
     * Get the span context for parent-child relationships
     */
    [[nodiscard]] opentelemetry::context::Context getContext() const {
        return context_;
    }

    // Make span_ accessible to ObservabilityManager for parent-child
    // relationships
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
    explicit OperationSpan(
        opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span);
    ~OperationSpan();

    // No copy, move only
    OperationSpan(const OperationSpan &) = delete;
    OperationSpan &operator=(const OperationSpan &) = delete;
    OperationSpan(OperationSpan &&) = default;
    OperationSpan &operator=(OperationSpan &&) = default;

    /**
     * Mark the operation as successful
     */
    void setSuccess();

    /**
     * Mark the operation as failed with an error message
     */
    void setError(const std::string &errorMessage);

    /**
     * Add attributes
     */
    void setAttribute(const std::string &key, const std::string &value);
    void setAttribute(const std::string &key, int64_t value);
    void setAttribute(const std::string &key, double value);
    void setAttribute(const std::string &key, int value);
    void setAttribute(const std::string &key, uint8_t value);
    void setAttribute(const std::string &key, uint16_t value);
    void setAttribute(const std::string &key, uint32_t value);
    void setAttribute(const std::string &key, bool value);
    void setAttribute(const std::string &key, framenum_t value);

    /**
     * Record an exception
     */
    void recordException(const std::exception &ex);

    /**
     * Get the span context for parent-child relationships
     */
    [[nodiscard]] opentelemetry::context::Context getContext() const {
        return context_;
    }

    /**
     * Get the underlying span for creating child spans
     */
    [[nodiscard]] opentelemetry::trace::Span *getSpan() const {
        return span_.get();
    }

  private:
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span_;
    opentelemetry::context::Context context_;
    bool statusSet_;
};

// Convenience macro for creating request spans
#define CREATE_REQUEST_SPAN(observability_manager, method, url)                \
    auto span = observability_manager->createRequestSpan(method + " " + url,   \
                                                         method, url)

} // namespace creatures