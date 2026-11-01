#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "api/CreatureResponse.h"
#include "server/transport/HttpTypes.h"
#include "util/ObservabilityManager.h"
#include "util/Result.h"

namespace creatures::transport {

using CreatureListLoader =
    std::function<Result<std::vector<api::CreatureResponse>>(const std::shared_ptr<OperationSpan> &parentSpan)>;
using CreatureLoader = std::function<Result<api::CreatureResponse>(const creatureId_t &creatureId,
                                                                   const std::shared_ptr<OperationSpan> &parentSpan)>;

/** Framework-neutral implementation of GET /api/v1/creature. */
PreparedResponse listCreatures(const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse listCreatures(const std::shared_ptr<OperationSpan> &parentSpan, const CreatureListLoader &loader);

/** Framework-neutral implementation of GET /api/v1/creature/{creatureId}. */
PreparedResponse getCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse getCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &parentSpan,
                             const CreatureLoader &loader);

/** Framework-neutral implementation of PATCH /api/v1/creature/{creatureId}/idle. */
PreparedResponse setCreatureIdle(const creatureId_t &creatureId, const std::string &body,
                                 const std::shared_ptr<OperationSpan> &parentSpan);

PreparedResponse upsertCreature(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse validateCreature(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse registerCreature(const std::string &body, const std::shared_ptr<OperationSpan> &parentSpan);
PreparedResponse exportCreature(const creatureId_t &creatureId, const std::shared_ptr<OperationSpan> &parentSpan);

} // namespace creatures::transport
