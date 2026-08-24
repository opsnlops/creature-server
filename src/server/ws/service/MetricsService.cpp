

#include "MetricsService.h"

#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace creatures {
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures ::ws {

Result<SystemCountersSnapshot> MetricsService::getCounters(std::shared_ptr<RequestSpan> parentSpan) {
    auto span = creatures::observability
                    ? creatures::observability->createOperationSpan("MetricsService.getCounters", std::move(parentSpan))
                    : nullptr;
    if (span) {
        span->setAttribute("service", "MetricsService");
        span->setAttribute("operation", "getCounters");
    }

    if (!creatures::metrics) {
        constexpr std::string_view errorMessage = "Metrics object is null";
        spdlog::error("{}", errorMessage);
        if (span) {
            span->setError(std::string(errorMessage));
            span->setAttribute("error.type", "InternalError");
            span->setAttribute("error.code", static_cast<int64_t>(ServerError::InternalError));
            span->setAttribute("error.message", std::string(errorMessage));
        }
        return Result<SystemCountersSnapshot>{ServerError{ServerError::InternalError, std::string(errorMessage)}};
    }

    const auto snapshot = creatures::metrics->snapshot();
    if (span) {
        span->setAttribute("metrics.counter.count", static_cast<int64_t>(41));
        span->setSuccess();
    }
    return Result<SystemCountersSnapshot>{snapshot};
}

} // namespace creatures::ws
