#include "server/transport/CreatureReadHandlers.h"

#include "server/ws/service/CreatureService.h"

namespace creatures::transport {

PreparedResponse listCreatures(const std::shared_ptr<OperationSpan> &requestSpan) {
    return listCreatures(requestSpan, [](const std::shared_ptr<OperationSpan> &span) {
        return ws::CreatureService::getAllCreaturesFromOperation(span);
    });
}

PreparedResponse getCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &requestSpan) {
    return getCreature(creatureId, requestSpan, [](const creatureId_t &id, const std::shared_ptr<OperationSpan> &span) {
        return ws::CreatureService::getCreatureFromOperation(id, span);
    });
}

} // namespace creatures::transport
