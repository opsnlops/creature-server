#include "server/transport/FixtureReadHandlers.h"

#include "server/ws/service/DmxFixtureService.h"

namespace creatures::transport {

PreparedResponse listFixtures(const std::shared_ptr<OperationSpan> &requestSpan) {
    return listFixtures(requestSpan, [](const std::shared_ptr<OperationSpan> &span) {
        return ws::DmxFixtureService::getAllFixturesFromOperation(span);
    });
}

PreparedResponse getFixture(const fixtureId_t &fixtureId, const std::shared_ptr<OperationSpan> &requestSpan) {
    return getFixture(fixtureId, requestSpan, [](const fixtureId_t &id, const std::shared_ptr<OperationSpan> &span) {
        return ws::DmxFixtureService::getFixtureFromOperation(id, span);
    });
}

} // namespace creatures::transport
