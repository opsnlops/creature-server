#include "DmxFixtureService.h"

#include <memory>
#include <vector>

#include "server/database.h"
#include "util/ObservabilityManager.h"

namespace creatures {
extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
} // namespace creatures

namespace creatures::ws {

namespace {

const char *fixtureReadErrorType(ServerError::Code code) {
    switch (code) {
    case ServerError::NotFound:
        return "NotFound";
    case ServerError::InvalidData:
        return "InvalidData";
    case ServerError::DatabaseError:
        return "DatabaseError";
    default:
        return "InternalError";
    }
}

template <typename T> Result<T> fixtureReadError(const std::shared_ptr<OperationSpan> &span, const ServerError &error) {
    recordSpanError(span, error.getMessage(), fixtureReadErrorType(error.getCode()), error.getCode());
    return Result<T>{error};
}

Result<std::vector<DmxFixture>> getAllFixturesWithSpan(const std::shared_ptr<OperationSpan> &span) {
    if (!db) {
        return fixtureReadError<std::vector<DmxFixture>>(
            span, ServerError(ServerError::InternalError, "Database unavailable"));
    }
    auto result = db->getAllFixtures(span);
    if (!result.isSuccess()) {
        return fixtureReadError<std::vector<DmxFixture>>(span, result.getError().value());
    }

    const auto fixtures = result.getValue().value();
    if (span) {
        span->setAttribute("fixtures.count", static_cast<int64_t>(fixtures.size()));
        span->setSuccess();
    }
    return Result<std::vector<DmxFixture>>{fixtures};
}

Result<DmxFixture> getFixtureWithSpan(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &span) {
    if (span) {
        span->setAttribute("fixture.id", fixtureId);
    }
    if (fixtureId.empty()) {
        return fixtureReadError<DmxFixture>(span, ServerError(ServerError::InvalidData, "fixtureId is required"));
    }
    if (!db) {
        return fixtureReadError<DmxFixture>(span, ServerError(ServerError::InternalError, "Database unavailable"));
    }

    auto result = db->getFixture(fixtureId, span);
    if (!result.isSuccess()) {
        return fixtureReadError<DmxFixture>(span, result.getError().value());
    }
    if (!result.getValue().has_value()) {
        return fixtureReadError<DmxFixture>(
            span, ServerError(ServerError::InternalError, "Database returned no fixture value"));
    }

    auto fixture = result.getValue().value();
    if (span) {
        span->setAttribute("fixture.name", fixture.name);
        span->setAttribute("fixture.channel_count", static_cast<int64_t>(fixture.channels.size()));
        span->setSuccess();
    }
    return Result<DmxFixture>{fixture};
}

} // namespace

Result<std::vector<DmxFixture>> DmxFixtureService::getAllFixtures(std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("DmxFixtureService.getAllFixtures", parentSpan) : nullptr;

    return getAllFixturesWithSpan(span);
}

Result<std::vector<DmxFixture>>
DmxFixtureService::getAllFixturesFromOperation(std::shared_ptr<OperationSpan> parentSpan) {
    auto span = observability ? observability->createChildOperationSpan("DmxFixtureService.getAllFixtures", parentSpan)
                              : nullptr;
    return getAllFixturesWithSpan(span);
}

Result<DmxFixture> DmxFixtureService::getFixture(const fixtureId_t &fixtureId,
                                                 std::shared_ptr<RequestSpan> parentSpan) {
    auto span =
        observability ? observability->createOperationSpan("DmxFixtureService.getFixture", parentSpan) : nullptr;
    return getFixtureWithSpan(fixtureId, span);
}

Result<DmxFixture> DmxFixtureService::getFixtureFromOperation(const fixtureId_t &fixtureId,
                                                              std::shared_ptr<OperationSpan> parentSpan) {
    auto span =
        observability ? observability->createChildOperationSpan("DmxFixtureService.getFixture", parentSpan) : nullptr;
    return getFixtureWithSpan(fixtureId, span);
}

} // namespace creatures::ws
