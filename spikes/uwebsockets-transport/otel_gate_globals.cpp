#include <memory>

#include "model/DmxFixture.h"
#include "server/database.h"
#include "util/ObservabilityManager.h"
#include "util/cache.h"

namespace creatures {

std::shared_ptr<Database> db;
std::shared_ptr<ObservabilityManager> observability;
std::shared_ptr<ObjectCache<fixtureId_t, DmxFixture>> fixtureCache;

} // namespace creatures
