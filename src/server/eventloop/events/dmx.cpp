

#include <sstream>

#include <spdlog/spdlog.h>

#include <E131Server.h>

#include "server/config.h"
#include "server/eventloop/events/types.h"
#include "server/metrics/counters.h"
#include "util/Result.h"
#include "util/helpers.h"

#include "server/namespace-stuffs.h"

extern std::shared_ptr<creatures::EventLoop> eventLoop;

namespace creatures {

extern std::shared_ptr<creatures::e131::E131Server> e131Server;
extern std::shared_ptr<SystemCounters> metrics;

Result<framenum_t> DMXEvent::executeImpl() {

    // Send the DMX data
    e131Server->setValues(universe, channelOffset, data);

    // Update our metrics
    metrics->incrementDMXEventsProcessed();

#if DEBUG_EVENT_DMX
    debug("DMX data: Offset: {}, data: {}", channelOffset, vectorToHexString(data));
#endif

    return Result{this->frameNumber};
}

} // namespace creatures