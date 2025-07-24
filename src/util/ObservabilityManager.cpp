//
// Created by April White on 5/31/25.
//

#include "ObservabilityManager.h"

#include <opentelemetry/context/runtime_context.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_metric_exporter_options.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/sdk/metrics/export/periodic_exporting_metric_reader_factory.h>
#include <opentelemetry/sdk/metrics/meter_provider_factory.h>
#include <opentelemetry/sdk/metrics/push_metric_exporter.h>
#include <opentelemetry/sdk/resource/resource.h>
#include <opentelemetry/sdk/resource/semantic_conventions.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/simple_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>

#include "server/metrics/counters.h"
#include "server/namespace-stuffs.h"

namespace otlp = opentelemetry::exporter::otlp;
namespace trace_api = opentelemetry::trace;
namespace trace_sdk = opentelemetry::sdk::trace;
namespace metrics_api = opentelemetry::metrics;
namespace metrics_sdk = opentelemetry::sdk::metrics;
namespace resource = opentelemetry::sdk::resource;

namespace creatures {

ObservabilityManager::ObservabilityManager() : initialized_(false) {}

void ObservabilityManager::initialize(const std::string &serviceName,
                                      const std::string &serviceVersion,
                                      const std::string &honeycombApiKey,
                                      const std::string &honeycombDataset) {

    // Create resource attributes using semantic conventions
    auto resource_attributes = resource::ResourceAttributes{
        {resource::SemanticConventions::kServiceName, serviceName},
        {resource::SemanticConventions::kServiceVersion, serviceVersion}};
    auto resource = resource::Resource::Create(resource_attributes);

    // ============= TRACING SETUP =============

    // Configure OTLP HTTP exporter for Honeycomb traces
    otlp::OtlpHttpExporterOptions trace_exporter_options;
    if (!honeycombApiKey.empty()) {
        trace_exporter_options.url = "https://api.honeycomb.io/v1/traces";
        trace_exporter_options.http_headers.insert(
            {"x-honeycomb-team", honeycombApiKey});
        trace_exporter_options.http_headers.insert(
            {"x-honeycomb-dataset", honeycombDataset});
    } else {
        trace_exporter_options.url = "http://localhost:4318/v1/traces";
    }

    auto trace_exporter =
        otlp::OtlpHttpExporterFactory::Create(trace_exporter_options);
    auto trace_processor = trace_sdk::BatchSpanProcessorFactory::Create(
        std::move(trace_exporter), trace_sdk::BatchSpanProcessorOptions{});

    auto trace_provider_unique = trace_sdk::TracerProviderFactory::Create(
        std::move(trace_processor), resource);
    std::shared_ptr<trace_api::TracerProvider> trace_provider_std =
        std::move(trace_provider_unique);
    auto trace_provider_shared =
        opentelemetry::nostd::shared_ptr<trace_api::TracerProvider>(
            trace_provider_std);

    trace_api::Provider::SetTracerProvider(trace_provider_shared);
    tracer_ = trace_api::Provider::GetTracerProvider()->GetTracer(
        serviceName, serviceVersion);

    // ============= METRICS SETUP =============

    // Configure OTLP HTTP exporter for Honeycomb metrics
    otlp::OtlpHttpMetricExporterOptions metric_exporter_options;
    if (!honeycombApiKey.empty()) {
        metric_exporter_options.url = "https://api.honeycomb.io/v1/metrics";
        metric_exporter_options.http_headers.insert(
            {"x-honeycomb-team", honeycombApiKey});
        // Append -metrics to the dataset name for metrics data
        metric_exporter_options.http_headers.insert(
            {"x-honeycomb-dataset", honeycombDataset + "-metrics"});
    } else {
        metric_exporter_options.url = "http://localhost:4318/v1/metrics";
    }

    auto metric_exporter =
        otlp::OtlpHttpMetricExporterFactory::Create(metric_exporter_options);

    // We don't need a periodic reader since we'll export manually from the
    // event loop! This is much better for your 1ms timing requirements 🐰
    auto metric_reader =
        metrics_sdk::PeriodicExportingMetricReaderFactory::Create(
            std::move(metric_exporter),
            metrics_sdk::PeriodicExportingMetricReaderOptions{
                // Set export interval to something long since we'll trigger
                // manually
                .export_interval_millis =
                    std::chrono::milliseconds(60000), // 1 minute fallback
                .export_timeout_millis =
                    std::chrono::milliseconds(30000) // 30 second timeout
            });

    // Create MeterProvider with proper API for v1.21.0
    auto view_registry = std::make_unique<metrics_sdk::ViewRegistry>();
    auto metric_provider = metrics_sdk::MeterProviderFactory::Create(
        std::move(view_registry), resource);

    // Add the reader to the provider
    auto *provider_ptr =
        static_cast<metrics_sdk::MeterProvider *>(metric_provider.get());
    provider_ptr->AddMetricReader(std::move(metric_reader));

    // Convert std::unique_ptr to opentelemetry::nostd::shared_ptr
    std::shared_ptr<metrics_api::MeterProvider> provider_std =
        std::move(metric_provider);
    auto provider_shared =
        opentelemetry::nostd::shared_ptr<metrics_api::MeterProvider>(
            provider_std);

    metrics_api::Provider::SetMeterProvider(provider_shared);
    meter_ = metrics_api::Provider::GetMeterProvider()->GetMeter(
        serviceName, serviceVersion);

    // Initialize all metric instruments
    initializeMetricInstruments();

    initialized_ = true;

    info("ObservabilityManager initialized - ready to hop into action! 🐰");
}

void ObservabilityManager::initializeMetricInstruments() {
    if (!meter_) {
        warn("Meter not initialized, can't create metric instruments");
        return;
    }

    // Create all the counter instruments
    // Using Counter since these are monotonic increasing values
    totalFramesCounter_ = meter_->CreateUInt64Counter(
        "creature_server_total_frames",
        "Total number of frames processed by the event loop", "frames");

    eventsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_events_processed",
        "Total number of events processed by the event loop", "events");

    framesStreamedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_frames_streamed",
        "Total number of frames streamed from clients", "frames");

    dmxEventsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_dmx_events_processed",
        "Total number of DMX events processed", "events");

    animationsPlayedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_animations_played",
        "Total number of animations played", "animations");

    soundsPlayedCounter_ =
        meter_->CreateUInt64Counter("creature_server_sounds_played",
                                    "Total number of sounds played", "sounds");

    playlistsStartedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_playlists_started",
        "Total number of playlists started", "playlists");

    playlistsStoppedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_playlists_stopped",
        "Total number of playlists stopped", "playlists");

    playlistsEventsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_playlist_events_processed",
        "Total number of playlist events processed", "events");

    playlistStatusRequestsCounter_ = meter_->CreateUInt64Counter(
        "creature_server_playlist_status_requests",
        "Total number of playlist status requests", "requests");

    restRequestsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_rest_requests_processed",
        "Total number of REST requests processed", "requests");

    rtpEventsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_rtp_events_processed",
        "Total number of RTP audio chunk events processed", "events");

    soundFilesServedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_sound_files_served",
        "Total number of sound files served", "files");

    websocketConnectionsProcessedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_websocket_connections_processed",
        "Total number of WebSocket connections processed", "connections");

    websocketMessagesReceivedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_websocket_messages_received",
        "Total number of WebSocket messages received", "messages");

    websocketMessagesSentCounter_ = meter_->CreateUInt64Counter(
        "creature_server_websocket_messages_sent",
        "Total number of WebSocket messages sent", "messages");

    websocketPingsSentCounter_ = meter_->CreateUInt64Counter(
        "creature_server_websocket_pings_sent",
        "Total number of WebSocket pings sent", "pings");

    websocketPongsReceivedCounter_ = meter_->CreateUInt64Counter(
        "creature_server_websocket_pongs_received",
        "Total number of WebSocket pongs received", "pongs");

    debug(
        "All metric instruments initialized - ready to count like a bunny! 🐰");
}

void ObservabilityManager::exportMetrics(
    const std::shared_ptr<SystemCounters> &metrics) {
    if (!initialized_ || !metrics) {
        return;
    }

    trace("Exporting metrics to OTel - hop to it! 🐰");

    // Since these are cumulative counters and OTel expects delta values,
    // we need to track the previous values and only send the difference
    static std::atomic<uint64_t> lastTotalFrames{0};
    static std::atomic<uint64_t> lastEventsProcessed{0};
    static std::atomic<uint64_t> lastFramesStreamed{0};
    static std::atomic<uint64_t> lastDmxEventsProcessed{0};
    static std::atomic<uint64_t> lastAnimationsPlayed{0};
    static std::atomic<uint64_t> lastSoundsPlayed{0};
    static std::atomic<uint64_t> lastPlaylistsStarted{0};
    static std::atomic<uint64_t> lastPlaylistsStopped{0};
    static std::atomic<uint64_t> lastPlaylistsEventsProcessed{0};
    static std::atomic<uint64_t> lastPlaylistStatusRequests{0};
    static std::atomic<uint64_t> lastRestRequestsProcessed{0};
    static std::atomic<uint64_t> lastRtpEventsProcessed{0};
    static std::atomic<uint64_t> lastSoundFilesServed{0};
    static std::atomic<uint64_t> lastWebsocketConnectionsProcessed{0};
    static std::atomic<uint64_t> lastWebsocketMessagesReceived{0};
    static std::atomic<uint64_t> lastWebsocketMessagesSent{0};
    static std::atomic<uint64_t> lastWebsocketPingsSent{0};
    static std::atomic<uint64_t> lastWebsocketPongsReceived{0};

    // Calculate deltas and send them
    uint64_t currentTotalFrames = metrics->getTotalFrames();
    uint64_t deltaTotalFrames =
        currentTotalFrames - lastTotalFrames.exchange(currentTotalFrames);
    if (deltaTotalFrames > 0)
        totalFramesCounter_->Add(deltaTotalFrames);

    uint64_t currentEventsProcessed = metrics->getEventsProcessed();
    uint64_t deltaEventsProcessed =
        currentEventsProcessed -
        lastEventsProcessed.exchange(currentEventsProcessed);
    if (deltaEventsProcessed > 0)
        eventsProcessedCounter_->Add(deltaEventsProcessed);

    uint64_t currentFramesStreamed = metrics->getFramesStreamed();
    uint64_t deltaFramesStreamed =
        currentFramesStreamed -
        lastFramesStreamed.exchange(currentFramesStreamed);
    if (deltaFramesStreamed > 0)
        framesStreamedCounter_->Add(deltaFramesStreamed);

    uint64_t currentDmxEventsProcessed = metrics->getDMXEventsProcessed();
    uint64_t deltaDmxEventsProcessed =
        currentDmxEventsProcessed -
        lastDmxEventsProcessed.exchange(currentDmxEventsProcessed);
    if (deltaDmxEventsProcessed > 0)
        dmxEventsProcessedCounter_->Add(deltaDmxEventsProcessed);

    uint64_t currentAnimationsPlayed = metrics->getAnimationsPlayed();
    uint64_t deltaAnimationsPlayed =
        currentAnimationsPlayed -
        lastAnimationsPlayed.exchange(currentAnimationsPlayed);
    if (deltaAnimationsPlayed > 0)
        animationsPlayedCounter_->Add(deltaAnimationsPlayed);

    uint64_t currentSoundsPlayed = metrics->getSoundsPlayed();
    uint64_t deltaSoundsPlayed =
        currentSoundsPlayed - lastSoundsPlayed.exchange(currentSoundsPlayed);
    if (deltaSoundsPlayed > 0)
        soundsPlayedCounter_->Add(deltaSoundsPlayed);

    uint64_t currentPlaylistsStarted = metrics->getPlaylistsStarted();
    uint64_t deltaPlaylistsStarted =
        currentPlaylistsStarted -
        lastPlaylistsStarted.exchange(currentPlaylistsStarted);
    if (deltaPlaylistsStarted > 0)
        playlistsStartedCounter_->Add(deltaPlaylistsStarted);

    uint64_t currentPlaylistsStopped = metrics->getPlaylistsStopped();
    uint64_t deltaPlaylistsStopped =
        currentPlaylistsStopped -
        lastPlaylistsStopped.exchange(currentPlaylistsStopped);
    if (deltaPlaylistsStopped > 0)
        playlistsStoppedCounter_->Add(deltaPlaylistsStopped);

    uint64_t currentPlaylistsEventsProcessed =
        metrics->getPlaylistsEventsProcessed();
    uint64_t deltaPlaylistsEventsProcessed =
        currentPlaylistsEventsProcessed -
        lastPlaylistsEventsProcessed.exchange(currentPlaylistsEventsProcessed);
    if (deltaPlaylistsEventsProcessed > 0)
        playlistsEventsProcessedCounter_->Add(deltaPlaylistsEventsProcessed);

    uint64_t currentPlaylistStatusRequests =
        metrics->getPlaylistStatusRequests();
    uint64_t deltaPlaylistStatusRequests =
        currentPlaylistStatusRequests -
        lastPlaylistStatusRequests.exchange(currentPlaylistStatusRequests);
    if (deltaPlaylistStatusRequests > 0)
        playlistStatusRequestsCounter_->Add(deltaPlaylistStatusRequests);

    uint64_t currentRestRequestsProcessed = metrics->getRestRequestsProcessed();
    uint64_t deltaRestRequestsProcessed =
        currentRestRequestsProcessed -
        lastRestRequestsProcessed.exchange(currentRestRequestsProcessed);
    if (deltaRestRequestsProcessed > 0)
        restRequestsProcessedCounter_->Add(deltaRestRequestsProcessed);

    uint64_t currentRtpEventsProcessed = metrics->getRtpEventsProcessed();
    uint64_t deltaRtpEventsProcessed =
        currentRtpEventsProcessed -
        lastRtpEventsProcessed.exchange(currentRtpEventsProcessed);
    if (deltaRtpEventsProcessed > 0)
        rtpEventsProcessedCounter_->Add(deltaRtpEventsProcessed);

    uint64_t currentSoundFilesServed = metrics->getSoundFilesServed();
    uint64_t deltaSoundFilesServed =
        currentSoundFilesServed -
        lastSoundFilesServed.exchange(currentSoundFilesServed);
    if (deltaSoundFilesServed > 0)
        soundFilesServedCounter_->Add(deltaSoundFilesServed);

    uint64_t currentWebsocketConnectionsProcessed =
        metrics->getWebsocketConnectionsProcessed();
    uint64_t deltaWebsocketConnectionsProcessed =
        currentWebsocketConnectionsProcessed -
        lastWebsocketConnectionsProcessed.exchange(
            currentWebsocketConnectionsProcessed);
    if (deltaWebsocketConnectionsProcessed > 0)
        websocketConnectionsProcessedCounter_->Add(
            deltaWebsocketConnectionsProcessed);

    uint64_t currentWebsocketMessagesReceived =
        metrics->getWebsocketMessagesReceived();
    uint64_t deltaWebsocketMessagesReceived =
        currentWebsocketMessagesReceived -
        lastWebsocketMessagesReceived.exchange(
            currentWebsocketMessagesReceived);
    if (deltaWebsocketMessagesReceived > 0)
        websocketMessagesReceivedCounter_->Add(deltaWebsocketMessagesReceived);

    uint64_t currentWebsocketMessagesSent = metrics->getWebsocketMessagesSent();
    uint64_t deltaWebsocketMessagesSent =
        currentWebsocketMessagesSent -
        lastWebsocketMessagesSent.exchange(currentWebsocketMessagesSent);
    if (deltaWebsocketMessagesSent > 0)
        websocketMessagesSentCounter_->Add(deltaWebsocketMessagesSent);

    uint64_t currentWebsocketPingsSent = metrics->getWebsocketPingsSent();
    uint64_t deltaWebsocketPingsSent =
        currentWebsocketPingsSent -
        lastWebsocketPingsSent.exchange(currentWebsocketPingsSent);
    if (deltaWebsocketPingsSent > 0)
        websocketPingsSentCounter_->Add(deltaWebsocketPingsSent);

    uint64_t currentWebsocketPongsReceived =
        metrics->getWebsocketPongsReceived();
    uint64_t deltaWebsocketPongsReceived =
        currentWebsocketPongsReceived -
        lastWebsocketPongsReceived.exchange(currentWebsocketPongsReceived);
    if (deltaWebsocketPongsReceived > 0)
        websocketPongsReceivedCounter_->Add(deltaWebsocketPongsReceived);

    debug("Metrics exported to OTel");
}

std::shared_ptr<RequestSpan>
ObservabilityManager::createRequestSpan(const std::string &operationName,
                                        const std::string &httpMethod,
                                        const std::string &httpUrl) {

    if (!initialized_) {
        return nullptr;
    }

    auto span = tracer_->StartSpan(operationName);
    return std::make_shared<RequestSpan>(span, httpMethod, httpUrl);
}

std::shared_ptr<OperationSpan> ObservabilityManager::createOperationSpan(
    const std::string &operationName, std::shared_ptr<RequestSpan> parentSpan) {

    if (!initialized_) {
        return nullptr;
    }

    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span;

    if (parentSpan) {
        debug("creating child operation span for parent span with operation "
              "name: {}",
              operationName);
        auto spanContext = parentSpan->getSpan()->GetContext();
        auto options = opentelemetry::trace::StartSpanOptions{};
        options.parent = spanContext;
        span = tracer_->StartSpan(operationName, options);
    } else {
        debug("creating root operation span: {}", operationName);
        span = tracer_->StartSpan(operationName);
    }

    return std::make_shared<OperationSpan>(span);
}

std::shared_ptr<OperationSpan> ObservabilityManager::createChildOperationSpan(
    const std::string &operationName,
    std::shared_ptr<OperationSpan> parentSpan) {

    if (!initialized_ || !parentSpan) {
        return nullptr;
    }

    auto spanContext = parentSpan->getSpan()->GetContext();
    auto options = opentelemetry::trace::StartSpanOptions{};
    options.parent = spanContext;
    auto span = tracer_->StartSpan(operationName, options);

    return std::make_shared<OperationSpan>(span);
}

// RequestSpan implementation (unchanged from your original)
RequestSpan::RequestSpan(
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
    const std::string &httpMethod, const std::string &httpUrl)
    : span_(span), statusSet_(false) {

    context_ = opentelemetry::context::RuntimeContext::GetCurrent();
    span_->SetAttribute("http.method", httpMethod);
    span_->SetAttribute("http.url", httpUrl);
    span_->SetAttribute("component", "creature-server");
}

RequestSpan::~RequestSpan() {
    if (span_ && !statusSet_) {
        setHttpStatus(200);
    }
    if (span_) {
        span_->End();
    }
}

void RequestSpan::setHttpStatus(int statusCode) {
    if (!span_)
        return;

    span_->SetAttribute("http.status_code", static_cast<int64_t>(statusCode));

    if (statusCode >= 200 && statusCode < 400) {
        span_->SetStatus(trace_api::StatusCode::kOk);
    } else if (statusCode >= 400 && statusCode < 500) {
        span_->SetStatus(trace_api::StatusCode::kError, "Client Error");
    } else if (statusCode >= 500) {
        span_->SetStatus(trace_api::StatusCode::kError, "Server Error");
    }

    statusSet_ = true;
}

void RequestSpan::setAttribute(const std::string &key,
                               const std::string &value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void RequestSpan::setAttribute(const std::string &key, int64_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void RequestSpan::setAttribute(const std::string &key, bool value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void RequestSpan::recordException(const std::exception &ex) {
    if (span_) {
        span_->AddEvent("exception", {{"exception.type", typeid(ex).name()},
                                      {"exception.message", ex.what()}});
        span_->SetStatus(trace_api::StatusCode::kError, ex.what());
        statusSet_ = true;
    }
}

// OperationSpan implementation (unchanged from your original)
OperationSpan::OperationSpan(
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span)
    : span_(span), statusSet_(false) {
    context_ = opentelemetry::context::RuntimeContext::GetCurrent();
}

OperationSpan::~OperationSpan() {
    if (span_ && !statusSet_) {
        setSuccess();
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

void OperationSpan::setError(const std::string &errorMessage) {
    if (span_) {
        span_->SetStatus(trace_api::StatusCode::kError, errorMessage);
        statusSet_ = true;
    }
}

void OperationSpan::setAttribute(const std::string &key,
                                 const std::string &value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, int64_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, int value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, double value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, uint8_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, uint16_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, uint32_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}
void OperationSpan::setAttribute(const std::string &key, framenum_t value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::setAttribute(const std::string &key, bool value) {
    if (span_)
        span_->SetAttribute(key, value);
}

void OperationSpan::recordException(const std::exception &ex) {
    if (span_) {
        span_->AddEvent("exception", {{"exception.type", typeid(ex).name()},
                                      {"exception.message", ex.what()}});
        span_->SetStatus(trace_api::StatusCode::kError, ex.what());
        statusSet_ = true;
    }
}

} // namespace creatures