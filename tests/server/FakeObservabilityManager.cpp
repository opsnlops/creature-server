#include "util/ObservabilityManager.h"

namespace creatures {

ObservabilityManager::ObservabilityManager() = default;

void ObservabilityManager::initialize(const std::string &, const std::string &, const std::string &,
                                      const std::string &) {}

std::shared_ptr<RequestSpan> ObservabilityManager::createRequestSpan(const std::string &, const std::string &,
                                                                     const std::string &) {
    return nullptr;
}

std::shared_ptr<OperationSpan> ObservabilityManager::createOperationSpan(const std::string &,
                                                                         std::shared_ptr<RequestSpan>) {
    return nullptr;
}

std::shared_ptr<OperationSpan> ObservabilityManager::createChildOperationSpan(const std::string &,
                                                                              std::shared_ptr<OperationSpan>) {
    return nullptr;
}

std::shared_ptr<SamplingSpan> ObservabilityManager::createSamplingSpan(const std::string &, double) { return nullptr; }

std::shared_ptr<SamplingSpan> ObservabilityManager::createSamplingSpan(const std::string &,
                                                                       std::shared_ptr<OperationSpan>, double) {
    return nullptr;
}

void ObservabilityManager::exportMetrics(const std::shared_ptr<class SystemCounters> &) {}

void ObservabilityManager::exportSensorMetrics(const std::shared_ptr<class SensorDataCache> &) {}

} // namespace creatures
