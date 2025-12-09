#include "util/ObservabilityManager.h"

namespace creatures {

RequestSpan::RequestSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span,
                         const std::string & /*httpMethod*/, const std::string & /*httpUrl*/)
    : span_(std::move(span)), statusSet_(false) {}

RequestSpan::~RequestSpan() = default;

void RequestSpan::setHttpStatus(int /*statusCode*/) { statusSet_ = true; }

void RequestSpan::setAttribute(const std::string & /*key*/, const std::string & /*value*/) {}
void RequestSpan::setAttribute(const std::string & /*key*/, int64_t /*value*/) {}
void RequestSpan::setAttribute(const std::string & /*key*/, bool /*value*/) {}

void RequestSpan::recordException(const std::exception & /*ex*/) {}

OperationSpan::OperationSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span)
    : span_(std::move(span)), statusSet_(false) {}

OperationSpan::~OperationSpan() = default;

void OperationSpan::setSuccess() { statusSet_ = true; }
void OperationSpan::setError(const std::string & /*errorMessage*/) { statusSet_ = true; }

void OperationSpan::setAttribute(const std::string & /*key*/, const std::string & /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, int64_t /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, double /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, int /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, uint8_t /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, uint16_t /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, uint32_t /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, bool /*value*/) {}
void OperationSpan::setAttribute(const std::string & /*key*/, framenum_t /*value*/) {}

void OperationSpan::recordException(const std::exception & /*ex*/) { statusSet_ = true; }

SamplingSpan::SamplingSpan(opentelemetry::nostd::shared_ptr<opentelemetry::trace::Span> span, double /*samplingRate*/,
                           bool /*shouldExport*/,
                           opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> /*tracer*/)
    : OperationSpan(std::move(span)) {}

SamplingSpan::~SamplingSpan() = default;

void SamplingSpan::setSuccess() { OperationSpan::setSuccess(); }
void SamplingSpan::setError(const std::string &errorMessage) { OperationSpan::setError(errorMessage); }
void SamplingSpan::forceExport() {}

void SamplingSpan::setAttribute(const std::string &key, const std::string &value) {
    OperationSpan::setAttribute(key, value);
}
void SamplingSpan::setAttribute(const std::string &key, int64_t value) { OperationSpan::setAttribute(key, value); }
void SamplingSpan::setAttribute(const std::string &key, double value) { OperationSpan::setAttribute(key, value); }
void SamplingSpan::setAttribute(const std::string &key, bool value) { OperationSpan::setAttribute(key, value); }
void SamplingSpan::setAttribute(const std::string &key, framenum_t value) { OperationSpan::setAttribute(key, value); }

void SamplingSpan::recordException(const std::exception &ex) { OperationSpan::recordException(ex); }

} // namespace creatures
