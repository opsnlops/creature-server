
#pragma once

#include <memory>

#include "server/metrics/counters.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures ::ws {

class MetricsService {
  public:
    Result<SystemCountersSnapshot> getCounters(std::shared_ptr<RequestSpan> parentSpan = nullptr);
};

} // namespace creatures::ws
