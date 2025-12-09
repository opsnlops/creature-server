#pragma once

#include <memory>
#include <string>

#include "blockingconcurrentqueue.h"
#include "model/Creature.h"
#include "util/cache.h"

namespace creatures {
class Database;
class ObservabilityManager;
class EventLoop;
class SessionManager;
class Configuration;
class SystemCounters;
} // namespace creatures

namespace creatures {

extern std::shared_ptr<Database> db;
extern std::shared_ptr<ObservabilityManager> observability;
extern std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
extern std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
extern std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
extern std::shared_ptr<SystemCounters> metrics;
extern std::shared_ptr<EventLoop> eventLoop;
extern std::shared_ptr<SessionManager> sessionManager;
extern std::shared_ptr<Configuration> config;

} // namespace creatures
