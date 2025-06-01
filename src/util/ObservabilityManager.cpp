//
// Created by April White on 5/31/25.
//

#include "ObservabilityManager.h"

#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/resource/semantic_conventions.h>
#include <opentelemetry/context/runtime_context.h>

namespace otlp = opentelemetry::exporter::otlp;
namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace resource = opentelemetry::sdk::resource;

namespace creatures {

ObservabilityManager::ObservabilityManager() : initialized_(false) {}

void ObservabilityManager::initialize(const std::string& serviceName,
                                     const std::string& serviceVersion,
                                     const std::string& honeycombApiKey,
                                     const std::string& honeycombDataset) {

    // Create resource attributes using semantic conventions
    auto resource_attributes = resource::ResourceAttributes{
        {resource::SemanticConventions::kServiceName, serviceName},
        {resource::SemanticConventions::kServiceVersion, serviceVersion}
    };
    auto resource = resource::Resource::Create(resource_attributes);

    // Configure OTLP HTTP exporter for Honeycomb
    otlp::OtlpHttpExporterOptions exporter_options;

    if (!honeycombApiKey.empty()) {
        // Honeycomb configuration - use insert() for multimap
        exporter_options.url = "https://api.honeycomb.io/v1/traces";
        exporter_options.http_headers.insert({"x-honeycomb-team", honeycombApiKey});
        exporter_options.http_headers.insert({"x-honeycomb-dataset", honeycombDataset});
    } else {
        // Default to local collector
        exporter_options.url = "http://localhost:4318/v1/traces";
    }

    auto exporter = otlp::OtlpHttpExporterFactory::Create(exporter_options);

    // Use batch processor for better performance (won't block your 1ms event loop!)
    auto processor = trace_sdk::BatchSpanProcessorFactory::Create(
        std::move(exporter),
        trace_sdk::BatchSpanProcessorOptions{}
    );

    // Create the provider as a std::unique_ptr first
    auto provider_unique = trace_sdk::TracerProviderFactory::Create(
        std::move(processor), resource
    );

    // Convert to shared_ptr the simple way - let the compiler figure it out
    std::shared_ptr<trace_api::TracerProvider> provider_std = std::move(provider_unique);
    auto provider_shared = opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(provider_std);

    // Set the global trace provider
    trace_api::Provider::SetTracerProvider(provider_shared);

    // Get tracer for this service
    tracer_ = trace_api::Provider::GetTracerProvider()->GetTracer(
        serviceName, serviceVersion
    );

    initialized_ = true;
}

std::unique_ptr<RequestSpan> ObservabilityManager::createRequestSpan(
    const std::string& operationName,
    const std::string& httpMethod,
    const std::string& httpUrl) {

    if (!initialized_) {
        return nullptr;
    }

    auto span = tracer_->StartSpan(operationName);
    return std::make_unique<RequestSpan>(span, httpMethod, httpUrl);
}

std::unique_ptr<OperationSpan> ObservabilityManager::createOperationSpan(
    const std::string& operationName,
    const RequestSpan* parentSpan) {

    if (!initialized_) {
        return nullptr;
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span;

    if (parentSpan) {
        // Create child span - simple approach using span context
        auto spanContext = parentSpan->getSpan()->GetContext();
        auto options = opentelemetry::trace::StartSpanOptions{};
        options.parent = spanContext;
        span = tracer_->StartSpan(operationName, options);
    } else {
        // Create root span
        span = tracer_->StartSpan(operationName);
    }

    return std::make_unique<OperationSpan>(span);
}

std::unique_ptr<OperationSpan> ObservabilityManager::createChildOperationSpan(
    const std::string& operationName,
    const OperationSpan* parentSpan) {

    if (!initialized_ || !parentSpan) {
        return nullptr;
    }

    // Create child span using parent's span context
    auto spanContext = parentSpan->span_->GetContext();
    auto options = opentelemetry::trace::StartSpanOptions{};
    options.parent = spanContext;
    auto span = tracer_->StartSpan(operationName, options);

    return std::make_unique<OperationSpan>(span);
}

// RequestSpan implementation
RequestSpan::RequestSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
                        const std::string& httpMethod,
                        const std::string& httpUrl)
    : span_(span), statusSet_(false) {

    // Store the current context for parent-child relationships
    context_ = opentelemetry::context::RuntimeContext::GetCurrent();

    // Set standard HTTP attributes using string views for efficiency
    span_->SetAttribute("http.method", httpMethod);
    span_->SetAttribute("http.url", httpUrl);
    span_->SetAttribute("component", "creature-server");
}

RequestSpan::~RequestSpan() {
    if (span_ && !statusSet_) {
        // Default to OK if status wasn't explicitly set
        setHttpStatus(200);
    }
    if (span_) {
        span_->End();
    }
}

void RequestSpan::setHttpStatus(int statusCode) {
    if (!span_) return;

    span_->SetAttribute("http.status_code", static_cast<int64_t>(statusCode));

    // Set span status based on HTTP status
    if (statusCode >= 200 && statusCode < 400) {
        span_->SetStatus(trace_api::StatusCode::kOk);
    } else if (statusCode >= 400 && statusCode < 500) {
        span_->SetStatus(trace_api::StatusCode::kError, "Client Error");
    } else if (statusCode >= 500) {
        span_->SetStatus(trace_api::StatusCode::kError, "Server Error");
    }

    statusSet_ = true;
}

void RequestSpan::setAttribute(const std::string& key, const std::string& value) {
    if (span_) span_->SetAttribute(key, value);
}

void RequestSpan::setAttribute(const std::string& key, int64_t value) {
    if (span_) span_->SetAttribute(key, value);
}

void RequestSpan::setAttribute(const std::string& key, bool value) {
    if (span_) span_->SetAttribute(key, value);
}

void RequestSpan::recordException(const std::exception& ex) {
    if (span_) {
        // In v1.21.0, use AddEvent for exceptions
        span_->AddEvent("exception", {
            {"exception.type", typeid(ex).name()},
            {"exception.message", ex.what()}
        });
        span_->SetStatus(trace_api::StatusCode::kError, ex.what());
        statusSet_ = true;
    }
}

// OperationSpan implementation
OperationSpan::OperationSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span)
    : span_(span), statusSet_(false) {

    // Store the current context for parent-child relationships
    context_ = opentelemetry::context::RuntimeContext::GetCurrent();
}

OperationSpan::~OperationSpan() {
    if (span_ && !statusSet_) {
        setSuccess(); // Default to success
    }
    if (span_) {
        span_->End();
    }
}

void OperationSpan::setSuccess() {
    if (span_) {
        span_->SetStatus(trace_api::StatusCode::kOk);
        statusSet_ = true;
    }
}

void OperationSpan::setError(const std::string& errorMessage) {
    if (span_) {
        span_->SetStatus(trace_api::StatusCode::kError, errorMessage);
        statusSet_ = true;
    }
}

void OperationSpan::setAttribute(const std::string& key, const std::string& value) {
    if (span_) span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string& key, int64_t value) {
    if (span_) span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string& key, bool value) {
    if (span_) span_->SetAttribute(key, value);
}

void OperationSpan::recordException(const std::exception& ex) {
    if (span_) {
        // In v1.21.0, use AddEvent for exceptions
        span_->AddEvent("exception", {
            {"exception.type", typeid(ex).name()},
            {"exception.message", ex.what()}
        });
        span_->SetStatus(trace_api::StatusCode::kError, ex.what());
        statusSet_ = true;
    }
}

} // namespace creatures