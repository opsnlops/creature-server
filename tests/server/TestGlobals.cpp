#include "TestGlobals.h"

namespace creatures {

std::shared_ptr<Database> db;
std::shared_ptr<ObservabilityManager> observability;
std::shared_ptr<ObjectCache<creatureId_t, Creature>> creatureCache;
std::shared_ptr<ObjectCache<creatureId_t, universe_t>> creatureUniverseMap;
std::shared_ptr<moodycamel::BlockingConcurrentQueue<std::string>> websocketOutgoingMessages;
std::shared_ptr<SystemCounters> metrics;
std::shared_ptr<EventLoop> eventLoop;
std::shared_ptr<SessionManager> sessionManager;
std::shared_ptr<Configuration> config;

} // namespace creatures
