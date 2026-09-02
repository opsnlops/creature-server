#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "model/DmxFixture.h"
#include "server/transport/HttpTypes.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::transport {

using FixtureListLoader =
    std::function<Result<std::vector<DmxFixture>>(const std::shared_ptr<OperationSpan> &parentSpan)>;
using FixtureLoader =
    std::function<Result<DmxFixture>(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan)>;

/** Framework-neutral implementation of GET /api/v1/fixture. */
PreparedResponse listFixtures(const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse listFixtures(const std::shared_ptr<OperationSpan> &parentSpan, const FixtureListLoader &loader);

/** Framework-neutral implementation of GET /api/v1/fixture/{fixtureId}. */
PreparedResponse getFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse getFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan,
                            const FixtureLoader &loader);

} // namespace creatures::transport
