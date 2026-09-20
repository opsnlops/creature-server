#pragma once

#include <memory>
#include <string>

#include "model/DmxFixture.h"
#include "server/transport/HttpTypes.h"
#include "util/ObservabilityManager.h"

namespace creatures::transport {

PreparedResponse upsertFixture(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse validateFixture(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse deleteFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse setFixtureUniverse(const fixtureId_t &fixtureId, const std::string &body,
                                    const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse clearFixtureUniverse(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse triggerFixturePattern(const fixtureId_t &fixtureId, const std::string &patternId,
                                       const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse previewFixturePattern(const fixtureId_t &fixtureId, const std::string &body,
                                       const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse setFixtureLive(const fixtureId_t &fixtureId, const std::string &body,
                                const std::shared_ptr<OperationSpan> &parentSpan);

} // namespace creatures::transport
